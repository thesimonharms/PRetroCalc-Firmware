/* PRetroCalc OS - keyboard driver (STM32 BIOS over I2C1 @ 0x1F) */
#include "keyboard.h"
#include "board.h"
#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include <string.h>

#define FIFO_SIZE 64

static kbd_event_t fifo[FIFO_SIZE];
static volatile int fifo_head = 0, fifo_tail = 0;

static bool shift_held = false;
static bool sym_held   = false;   /* SYM key -> symbol layer */
static bool alt_held   = false;
static bool ctrl_held  = false;
static bool caps_lock  = false;

static int  key_repeat_code = -1;
static absolute_time_t repeat_next;

static bool i2c_ok = false;

static void fifo_push(uint8_t code, uint8_t type) {
    int next = (fifo_head + 1) % FIFO_SIZE;
    if (next == fifo_tail) return; /* drop on overflow */
    fifo[fifo_head].code = code;
    fifo[fifo_head].type = type;
    fifo_head = next;
}

/* Symbol layer produced when SYM is held (PicoCalc printed symbols) */
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
        case 'n': return ','; case 'm': return '.';
        case '0': return '='; /* space+sym etc. fall through */
        default:  return 0;
    }
}

/* I2C timeout: at 10kHz a 2-byte transaction takes ~2ms, but the STM32 BIOS
 * can stretch the clock while it services other interrupts. The reference
 * driver uses 500ms; 100ms was too aggressive and caused spurious failures
 * (which read back as 0% battery). */
#define KBD_I2C_TIMEOUT_US 500000

void kbd_init(void) {
    gpio_set_function(KBD_PIN_SDA, GPIO_FUNC_I2C);
    gpio_set_function(KBD_PIN_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(KBD_PIN_SDA);
    gpio_pull_up(KBD_PIN_SCL);
    i2c_init(KBD_I2C_MOD, KBD_I2C_SPEED);
    i2c_ok = true;
    /* enable "mods reported separately + applied" in the BIOS config */
    uint8_t msg[2] = { (uint8_t)(0x02 | 0x80), 0xD0 }; /* CFG: REPORT_MODS|USE_MODS|KEY_INT */
    i2c_write_timeout_us(KBD_I2C_MOD, KBD_I2C_ADDR, msg, 2, false, KBD_I2C_TIMEOUT_US);
}

static int kbd_reg_read16(uint8_t reg, uint16_t *out) {
    if (!i2c_ok) return -1;
    int r = i2c_write_timeout_us(KBD_I2C_MOD, KBD_I2C_ADDR, &reg, 1, true, KBD_I2C_TIMEOUT_US);
    if (r < 0) return r;
    return i2c_read_timeout_us(KBD_I2C_MOD, KBD_I2C_ADDR, (uint8_t *)out, 2, false, KBD_I2C_TIMEOUT_US);
}

void kbd_reg_write(uint8_t reg, uint8_t val) {
    if (!i2c_ok) return;
    uint8_t msg[2] = { (uint8_t)(reg | 0x80), val };
    i2c_write_timeout_us(KBD_I2C_MOD, KBD_I2C_ADDR, msg, 2, false, KBD_I2C_TIMEOUT_US);
}

void kbd_set_backlight(uint8_t v)  { kbd_reg_write(REG_ID_BK2, v); }
void kbd_set_lcd_backlight(uint8_t v) { kbd_reg_write(REG_ID_BKL, v); }

int kbd_battery_percent(void) {
    uint16_t v = 0;
    /* retry once on transient I2C failure before giving up */
    for (int attempt = 0; attempt < 2; attempt++) {
        if (kbd_reg_read16(REG_ID_BAT, &v) >= 0) {
            int pct = v & 0xFF;
            if (pct >= 0 && pct <= 100) return pct;
        }
    }
    return -1;
}

void kbd_power_off(void) {
    kbd_reg_write(REG_ID_OFF, 1);
}

static void emit(uint8_t c) {
    uint8_t out = c;
    /* printable ascii: apply shift/caps/sym layers */
    if (c >= 'a' && c <= 'z') {
        if (ctrl_held)      out = c - 'a' + 1;
        else if (sym_held)  { uint8_t s = sym_map(c); out = s ? s : c; }
        else if (shift_held ^ caps_lock) out = c - 'a' + 'A';
    } else if (c >= '0' && c <= '9') {
        if (shift_held) {
            const char *s = "!@#$%^&*()";
            out = s[c - '0'];
        } else if (sym_held) {
            uint8_t s = sym_map(c); if (s) out = s;
        }
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

    bool pressed = (state == 1 || state == 2); /* pressed / hold */

    switch (code) {
        case KMOD_SHL: case KMOD_SHR: shift_held = pressed; return;
        case KMOD_ALT:                alt_held   = pressed; return;
        case KMOD_SYM:                sym_held   = pressed; return;
        case KMOD_CTRL:               ctrl_held  = pressed; return;
        case KEY_CAPS_LOCK:
            if (state == 1) caps_lock = !caps_lock;
            return;
        default: break;
    }

    if (state == 1) { /* press */
        emit(code);
        key_repeat_code = code;
        repeat_next = make_timeout_time_ms(450);
    } else if (state == 3) { /* release */
        fifo_push(code, KBD_EV_RELEASE);
        if (key_repeat_code == code) key_repeat_code = -1;
    }
}

void kbd_poll(void) {
    uint16_t buff = 0;
    /* drain BIOS FIFO */
    for (int i = 0; i < 8; i++) {
        if (kbd_reg_read16(REG_ID_FIF, &buff) < 0) break;
        if (buff == 0) break;
        process_raw(buff);
    }
    /* auto-repeat */
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
    if (!kbd_get_event(&ev)) return -1;
    if (ev.type != KBD_EV_PRESS) return -1;
    return ev.code;
}

bool kbd_shift_held(void) { return shift_held; }
bool kbd_ctrl_held(void)  { return ctrl_held; }
bool kbd_alt_held(void)   { return alt_held; }
