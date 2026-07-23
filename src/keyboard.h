#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stdint.h>
#include <stdbool.h>

/* Event types */
#define KBD_EV_PRESS    1
#define KBD_EV_RELEASE  3

typedef struct {
    uint8_t code;   /* ascii code or KEY_* constant */
    uint8_t type;   /* KBD_EV_PRESS / KBD_EV_RELEASE */
} kbd_event_t;

/* Special keys reported by the STM32 BIOS */
#define KEY_JOY_UP     0x01
#define KEY_JOY_DOWN   0x02
#define KEY_JOY_LEFT   0x03
#define KEY_JOY_RIGHT  0x04
#define KEY_JOY_CENTER 0x05
#define KEY_BTN_LEFT1  0x06
#define KEY_BTN_RIGHT1 0x07
#define KEY_BACKSPACE  0x08
#define KEY_TAB        0x09
#define KEY_ENTER      0x0A
#define KEY_ESC        0xB1
#define KEY_UP         0xB5
#define KEY_DOWN       0xB6
#define KEY_LEFT       0xB4
#define KEY_RIGHT      0xB7
#define KEY_HOME       0xD2
#define KEY_DEL        0xD4
#define KEY_END        0xD5
#define KEY_PAGE_UP    0xD6
#define KEY_PAGE_DOWN  0xD7
#define KEY_CAPS_LOCK  0xC1
#define KEY_F1  0x81
#define KEY_F2  0x82
#define KEY_F3  0x83
#define KEY_F4  0x84
#define KEY_F5  0x85
#define KEY_F6  0x86
#define KEY_F7  0x87
#define KEY_F8  0x88
#define KEY_F9  0x89
#define KEY_F10 0x90
#define KEY_POWER 0x91

/* Modifier keys (reported raw) */
#define KMOD_ALT  0xA1
#define KMOD_SHL  0xA2
#define KMOD_SHR  0xA3
#define KMOD_SYM  0xA4
#define KMOD_CTRL 0xA5

void kbd_init(void);
void kbd_poll(void);                     /* call frequently; drains BIOS FIFO into OS queue */
bool kbd_get_event(kbd_event_t *ev);     /* pop next cooked event */
int  kbd_getchar(void);                  /* pop next press char or -1 */
bool kbd_shift_held(void);
bool kbd_ctrl_held(void);
bool kbd_alt_held(void);

void kbd_reg_write(uint8_t reg, uint8_t val);
void kbd_set_backlight(uint8_t v);       /* keyboard backlight 0-255 */
void kbd_set_lcd_backlight(uint8_t v);   /* LCD backlight 0-255 */
int  kbd_battery_percent(void);
void kbd_power_off(void);

#endif
