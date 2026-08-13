#include <Arduino.h>
#include <Wire.h>
#include <string.h>
#include "board.h"
#include "pico/stdlib.h"

extern "C" {
#include "keyboard.h"
}

#define FIFO_SIZE 64
#define KBD_I2C_TIMEOUT_MS 500

static kbd_event_t fifo[FIFO_SIZE];
static volatile int fifo_head, fifo_tail;
static bool shift_held, sym_held, alt_held, ctrl_held, caps_lock, i2c_ok;
static int key_repeat_code = -1;
static absolute_time_t repeat_next;

static void fifo_push(uint8_t code, uint8_t type) {
    int next = (fifo_head + 1) % FIFO_SIZE;
    if (next != fifo_tail) {
        fifo[fifo_head].code = code;
        fifo[fifo_head].type = type;
        fifo_head = next;
    }
}

static uint8_t sym_map(uint8_t c) {
    switch (c) {
        case 'q': return '#'; case 'w': return '1'; case 'e': return '2';
        case 'r': return '3'; case 't': return '('; case 'y': return ')';
        case 'u': return '_'; case 'i': return '-'; case 'o': return '+';
        case 'p': return '@'; case 'a': return '*'; case 's': return '4';
        case 'd': return '5'; case 'f': return '6'; case 'g': return '/';
        case 'h': return ':'; case 'j': return ';'; case 'k': return '\'';
        case 'l': return '"'; case 'z': return '7'; case 'x': return '8';
        case 'c': return '9'; case 'v': return '?'; case 'b': return '!';
        case 'n': return ','; case 'm': return '.'; case '0': return '=';
        default:  return 0;
    }
}

extern "C" {

void kbd_init(void) {
    Wire.begin(KBD_PIN_SDA, KBD_PIN_SCL, (uint32_t)KBD_I2C_SPEED);
    Wire.setClock((uint32_t)KBD_I2C_SPEED);
    Wire.setTimeOut(KBD_I2C_TIMEOUT_MS);
    i2c_ok = true;
    /* CFG: REPORT_MODS|USE_MODS|KEY_INT */
    Wire.beginTransmission(KBD_I2C_ADDR);
    Wire.write((uint8_t)(0x02 | 0x80));
    Wire.write((uint8_t)0xD0);
    Wire.endTransmission();
}

/* FIFO stays on a repeated start (low latency). Battery needs STOP + 16 ms
 * like ClockworkPi helloworld — otherwise the STM32 returns [0x0B, 0]. */
static int kbd_reg_read16(uint8_t reg, uint16_t *out, bool stop_and_wait) {
    if (!i2c_ok) return -1;
    Wire.beginTransmission(KBD_I2C_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(stop_and_wait) != 0) return -1;
    if (stop_and_wait) sleep_ms(16);
    if (Wire.requestFrom((int)KBD_I2C_ADDR, 2) != 2) return -1;
    uint8_t lo = (uint8_t)Wire.read();
    uint8_t hi = (uint8_t)Wire.read();
    *out = (uint16_t)lo | ((uint16_t)hi << 8);
    return 2;
}

void kbd_reg_write(uint8_t reg, uint8_t val) {
    if (!i2c_ok) return;
    Wire.beginTransmission(KBD_I2C_ADDR);
    Wire.write((uint8_t)(reg | 0x80));
    Wire.write(val);
    Wire.endTransmission();
}

void kbd_set_backlight(uint8_t v) { kbd_reg_write(REG_ID_BK2, v); }
void kbd_set_lcd_backlight(uint8_t v) { kbd_reg_write(REG_ID_BKL, v); }

int kbd_battery_percent(void) {
    uint16_t v = 0;
    for (int attempt = 0; attempt < 4; attempt++) {
        if (kbd_reg_read16(REG_ID_BAT, &v, true) >= 0 && v != 0) {
            int pct = (v >> 8) & 0x7F;
            if (pct <= 100) return pct;
        }
        sleep_ms(20);
    }
    return -1;
}

void kbd_power_off(void) { kbd_reg_write(REG_ID_OFF, 1); }

static void emit(uint8_t c) {
    uint8_t out = c;
    if (c >= 'a' && c <= 'z') {
        if (ctrl_held) out = c - 'a' + 1;
        else if (sym_held) { uint8_t s = sym_map(c); out = s ? s : c; }
        else if (shift_held ^ caps_lock) out = c - 'a' + 'A';
    } else if (c >= '0' && c <= '9') {
        if (shift_held) out = (uint8_t)"!@#$%^&*()"[c - '0'];
        else if (sym_held) { uint8_t s = sym_map(c); if (s) out = s; }
    } else if (shift_held) {
        switch (c) {
            case '-': out = '_'; break; case '=': out = '+'; break;
            case ',': out = '<'; break; case '.': out = '>'; break;
            case '/': out = '?'; break; case ';': out = ':'; break;
            case '\'': out = '"'; break; case '`': out = '~'; break;
            case '[': out = '{'; break; case ']': out = '}'; break;
            case '\\': out = '|'; break;
        }
    }
    fifo_push(out, KBD_EV_PRESS);
}

static void process_raw(uint16_t buff) {
    uint8_t state = buff & 0xFF;
    uint8_t code  = buff >> 8;
    if (buff == 0 || state == 0) return;
    bool pressed = (state == 1 || state == 2);
    switch (code) {
        case KMOD_SHL: case KMOD_SHR: shift_held = pressed; return;
        case KMOD_ALT:  alt_held = pressed; return;
        case KMOD_SYM:  sym_held = pressed; return;
        case KMOD_CTRL: ctrl_held = pressed; return;
        case KEY_CAPS_LOCK:
            if (state == 1) caps_lock = !caps_lock;
            return;
        default: break;
    }
    if (state == 1) {
        emit(code);
        key_repeat_code = code;
        repeat_next = make_timeout_time_ms(450);
    } else if (state == 3) {
        fifo_push(code, KBD_EV_RELEASE);
        if (key_repeat_code == code) key_repeat_code = -1;
    }
}

void kbd_poll(void) {
    uint16_t buff = 0;
    for (int i = 0; i < 8; i++) {
        if (kbd_reg_read16(REG_ID_FIF, &buff, false) < 0) break;
        if (buff == 0) break;
        process_raw(buff);
    }
    if (key_repeat_code >= 0 && time_reached(repeat_next)) {
        emit((uint8_t)key_repeat_code);
        repeat_next = make_timeout_time_ms(50);
    }
}

bool kbd_get_event(kbd_event_t *ev) {
    if (fifo_tail == fifo_head) return false;
    *ev = fifo[fifo_tail];
    fifo_tail = (fifo_tail + 1) % FIFO_SIZE;
    return true;
}

int kbd_getchar(void) {
    kbd_event_t ev;
    if (!kbd_get_event(&ev) || ev.type != KBD_EV_PRESS) return -1;
    return ev.code;
}

bool kbd_shift_held(void) { return shift_held; }
bool kbd_ctrl_held(void)  { return ctrl_held; }
bool kbd_alt_held(void)   { return alt_held; }

} /* extern "C" */
