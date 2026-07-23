/* PRetroCalc OS - built-in utility apps: calc, editor, files, monitor, settings, about */
#include "apps.h"
#include "os.h"
#include "gfx.h"
#include "keyboard.h"
#include "sound.h"
#include "sdfs.h"
#include "pscript.h"
#include "board.h"
#include "pico/stdlib.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <ctype.h>

/* ---------- shared GEM window ---------- */

static int gx, gy, gw, gh;   /* client rect from os_window */

static void draw_frame(const char *title) {
    os_gem_desktop_bg();
    os_window(title, &gx, &gy, &gw, &gh);
    gfx_puts_at(gx, gy + gh - 10, "ESC=close", GEM_DGRAY, GEM_WHITE);
}

static bool wait_key(int *code) {
    kbd_event_t ev;
    for (;;) {
        kbd_poll();
        sound_update();
        if (kbd_get_event(&ev) && ev.type == KBD_EV_PRESS) { *code = ev.code; return true; }
        sleep_ms(4);
    }
}

/* ================= CALCULATOR =================
 * Expression calculator with + - * / % ^ parentheses, decimals. */

static double parse_expr_r(const char **s);

static double parse_num(const char **s) {
    while (**s == ' ') (*s)++;
    if (**s == '(') { (*s)++; double v = parse_expr_r(s); if (**s == ')') (*s)++; return v; }
    if (**s == '-') { (*s)++; return -parse_num(s); }
    double v = 0;
    int got = 0;
    while (**s >= '0' && **s <= '9') { v = v * 10 + (**s - '0'); (*s)++; got = 1; }
    if (**s == '.') {
        (*s)++;
        double f = 0.1;
        while (**s >= '0' && **s <= '9') { v += f * (**s - '0'); f *= 0.1; (*s)++; }
    }
    (void)got;
    return v;
}

static double parse_pow(const char **s) {
    double base = parse_num(s);
    while (**s == ' ') (*s)++;
    if (**s == '^') { (*s)++; double e = parse_pow(s); double r = 1; for (int i = 0; i < (int)e; i++) r *= base; return r; }
    return base;
}

static double parse_term_r(const char **s) {
    double v = parse_pow(s);
    for (;;) {
        while (**s == ' ') (*s)++;
        if (**s == '*') { (*s)++; v *= parse_pow(s); }
        else if (**s == '/') { (*s)++; double d = parse_pow(s); v = d != 0 ? v / d : 0; }
        else if (**s == '%') { (*s)++; double d = parse_pow(s); v = d != 0 ? (double)((int)v % (int)d) : 0; }
        else return v;
    }
}

static double parse_expr_r(const char **s) {
    double v = parse_term_r(s);
    for (;;) {
        while (**s == ' ') (*s)++;
        if (**s == '+') { (*s)++; v += parse_term_r(s); }
        else if (**s == '-') { (*s)++; v -= parse_term_r(s); }
        else return v;
    }
}

void app_calc(void) {
    char expr[40] = {0};
    int len = 0;
    char result[40] = "";
    double mem = 0;
    for (;;) {
        draw_frame("CALC");
        gfx_puts_at(gx + 4, gy + 8, "EXPR:", GEM_DGRAY, GEM_WHITE);
        gfx_fill_rect(gx + 4, gy + 20, gw - 8, 18, GEM_LGRAY);
        gfx_rect(gx + 4, gy + 20, gw - 8, 18, GEM_BLACK);
        gfx_puts_at(gx + 8, gy + 25, expr, GEM_BLACK, GEM_LGRAY);
        gfx_puts_at(gx + 4, gy + 48, "=", GEM_DGRAY, GEM_WHITE);
        gfx_puts_at(gx + 20, gy + 48, result, GEM_GREEN, GEM_WHITE);
        char mb[40]; snprintf(mb, sizeof mb, "MEM: %.6g", mem);
        gfx_puts_at(gx + 4, gy + 66, mb, GEM_DGRAY, GEM_WHITE);
        gfx_puts_at(gx + 4, gy + 100, "ENTER: eval  C: clear", GEM_DGRAY, GEM_WHITE);
        gfx_puts_at(gx + 4, gy + 110, "M: store  R: recall", GEM_DGRAY, GEM_WHITE);
        gfx_puts_at(gx, gy + gh - 10, "ESC=close", GEM_DGRAY, GEM_WHITE);
        gfx_flush();

        int c; wait_key(&c);
        sound_click();
        if (c == KEY_ESC) return;
        if (c == KEY_BACKSPACE) { if (len > 0) expr[--len] = 0; }
        else if (c == KEY_ENTER) {
            const char *sp = expr;
            double v = parse_expr_r(&sp);
            if (v == (double)(long long)v) snprintf(result, sizeof result, "%lld", (long long)v);
            else snprintf(result, sizeof result, "%.8g", v);
        }
        else if (c == 'c' || c == 'C') { len = 0; expr[0] = 0; result[0] = 0; }
        else if (c == 'm' || c == 'M') { if (result[0]) mem = atof(result); }
        else if (c == 'r' || c == 'R') { snprintf(result, sizeof result, "%.8g", mem); }
        else if (len < 38 && (isdigit((uint8_t)c) || strchr("+-*/%^().", c))) {
            expr[len++] = c; expr[len] = 0;
        }
    }
}

/* ================= TEXT EDITOR ================= */

#define ED_MAX 4096
#define ED_COLS 40
#define ED_ROWS 36

static char ed_buf[ED_MAX];
static int ed_len = 0;
static int ed_cursor = 0;
static char ed_file[48] = "UNTITLED.TXT";

static void ed_render(int scroll) {
    draw_frame("EDIT");
    gfx_puts_at(gx + 50, gy - 17, ed_file, GEM_DGRAY, GEM_WHITE);
    int y = gy + 2;
    int pos = 0;
    int line = 0;
    /* compute start of display line = scroll */
    while (pos < ed_len && line < scroll) {
        if (ed_buf[pos] == '\n') line++;
        pos++;
    }
    int cursor_screen_x = 0, cursor_screen_y = 0;
    bool cursor_found = false;
    int x = gx;
    for (int i = pos; y < gy + gh - 14; i++) {
        if (i == ed_cursor) { cursor_screen_x = x; cursor_screen_y = y; cursor_found = true; }
        if (i >= ed_len) break;
        char ch = ed_buf[i];
        if (ch == '\n') { x = gx; y += FONT_H; continue; }
        if (x < gx + gw - FONT_W) gfx_glyph(x, y, ch, GEM_BLACK, GEM_WHITE);
        x += FONT_W;
        if (x > gx + gw - FONT_W) { x = gx; y += FONT_H; }
    }
    if (!cursor_found) { cursor_screen_x = x; cursor_screen_y = y; }
    gfx_fill_rect(cursor_screen_x, cursor_screen_y + FONT_H - 1, FONT_W, 1, GEM_GREEN);
}

void app_editor(void) {
    ed_len = 0; ed_cursor = 0; ed_buf[0] = 0;
    int scroll = 0;
    bool dirty = false;

    for (;;) {
        ed_render(scroll);
        gfx_flush();
        int c; wait_key(&c);
        sound_click();
        if (c == KEY_ESC) {
            if (dirty && os_sd_present) {
                /* save prompt: F2 = save */
                gfx_puts_at(gx, gy + gh - 10, "F2=save ESC=discard", GEM_GREEN, GEM_WHITE);
                gfx_flush();
                int c2; wait_key(&c2);
                if (c2 == KEY_F2) {
                    sdfs_write_file(ed_file, ed_buf, ed_len);
                }
            }
            return;
        }
        if (c == KEY_F2) { if (os_sd_present) { sdfs_write_file(ed_file, ed_buf, ed_len); dirty = false; } continue; }
        if (c == KEY_LEFT && ed_cursor > 0) ed_cursor--;
        else if (c == KEY_RIGHT && ed_cursor < ed_len) ed_cursor++;
        else if (c == KEY_UP) {
            int col = 0, p = ed_cursor;
            while (p > 0 && ed_buf[p-1] != '\n') { p--; col++; }
            if (p > 0) {
                p--;
                while (p > 0 && ed_buf[p-1] != '\n') p--;
                while (col-- > 0 && ed_buf[p] != '\n' && p < ed_len) p++;
                ed_cursor = p;
            }
            if (scroll > 0 && ed_cursor < scroll * 40) scroll--;
        }
        else if (c == KEY_DOWN) {
            int col = 0, p = ed_cursor;
            while (p > 0 && ed_buf[p-1] != '\n') { p--; col++; }
            while (p < ed_len && ed_buf[p] != '\n') p++;
            if (p < ed_len) {
                p++;
                while (col-- > 0 && ed_buf[p] != '\n' && p < ed_len) p++;
                ed_cursor = p;
            }
        }
        else if (c == KEY_BACKSPACE) {
            if (ed_cursor > 0) {
                memmove(&ed_buf[ed_cursor-1], &ed_buf[ed_cursor], ed_len - ed_cursor);
                ed_cursor--; ed_len--; ed_buf[ed_len] = 0; dirty = true;
            }
        }
        else if (c == KEY_ENTER) {
            if (ed_len < ED_MAX - 1) {
                memmove(&ed_buf[ed_cursor+1], &ed_buf[ed_cursor], ed_len - ed_cursor);
                ed_buf[ed_cursor++] = '\n'; ed_len++; ed_buf[ed_len] = 0; dirty = true;
            }
        }
        else if (c >= 32 && c < 127 && ed_len < ED_MAX - 1) {
            memmove(&ed_buf[ed_cursor+1], &ed_buf[ed_cursor], ed_len - ed_cursor);
            ed_buf[ed_cursor++] = c; ed_len++; ed_buf[ed_len] = 0; dirty = true;
        }
        /* keep cursor line visible */
        int line = 0;
        for (int i = 0; i < ed_cursor; i++) if (ed_buf[i] == '\n') line++;
        if (line < scroll) scroll = line;
        if (line >= scroll + ED_ROWS) scroll = line - ED_ROWS + 1;
    }
}

/* ================= FILE MANAGER ================= */

void app_files(void) {
    if (!os_sd_present) {
        draw_frame("FILES");
        gfx_puts_at(gx + 20, gy + 60, "No SD card inserted", GEM_BLACK, GEM_WHITE);
        gfx_flush();
        int c; wait_key(&c);
        return;
    }
    char names[24][48];
    int sel = 0;
    for (;;) {
        int n = sdfs_list_dir("/", names, 24, false);
        draw_frame("FILES  /");
        int y = gy + 4;
        for (int i = 0; i < n && i < 20; i++) {
            uint8_t fg = (i == sel) ? GEM_WHITE : GEM_BLACK;
            uint8_t bg = (i == sel) ? GEM_GREEN : GEM_WHITE;
            gfx_fill_rect(gx, y, gw, FONT_H, bg);
            gfx_puts_at(gx + 4, y, names[i], fg, bg);
            y += FONT_H;
        }
        if (n == 0) gfx_puts_at(gx + 20, gy + 40, "(empty card)", GEM_DGRAY, GEM_WHITE);
        gfx_puts_at(gx, gy + gh - 10, "ENTER=open/run  ESC=close", GEM_DGRAY, GEM_WHITE);
        gfx_flush();
        int c; wait_key(&c);
        sound_click();
        if (c == KEY_ESC) return;
        if (c == KEY_UP && sel > 0) sel--;
        if (c == KEY_DOWN && sel < n - 1) sel++;
        if (c == KEY_ENTER && n > 0) {
            /* run .PS scripts, view others */
            int l = strlen(names[sel]);
            if (l > 3 && !strcasecmp(names[sel] + l - 3, ".ps")) {
                static char buf[8192];
                uint32_t rl = 0;
                if (sdfs_read_file(names[sel], buf, sizeof(buf), &rl)) {
                    os_clear_screen();
                    pscript_run(buf);
                    gfx_puts_at(gx, gy + gh - 10, "Press any key...", GEM_DGRAY, GEM_WHITE);
                    gfx_flush();
                    wait_key(&c);
                }
            } else {
                static char buf[2048];
                uint32_t rl = 0;
                if (sdfs_read_file(names[sel], buf, sizeof(buf), &rl)) {
                    draw_frame(names[sel]);
                    int y = gy + 2, x = gx + 4;
                    for (uint32_t i = 0; i < rl && y < gy + gh - 12; i++) {
                        char ch = buf[i];
                        if (ch == '\n') { x = gx + 4; y += FONT_H; continue; }
                        if (ch >= 32 && ch < 127) gfx_glyph(x, y, ch, GEM_BLACK, GEM_WHITE);
                        x += FONT_W;
                        if (x > gx + gw - 12) { x = gx + 4; y += FONT_H; }
                    }
                    gfx_puts_at(gx, gy + gh - 10, "Press any key...", GEM_DGRAY, GEM_WHITE);
                    gfx_flush();
                    wait_key(&c);
                }
            }
        }
    }
}

/* ================= SYSTEM MONITOR ================= */

void app_monitor(void) {
    for (;;) {
        draw_frame("SYS MONITOR");
        int bat = kbd_battery_percent();
        char buf[64];
        snprintf(buf, sizeof buf, "Battery : %d%%", bat);
        gfx_puts_at(gx + 8, gy + 8, buf, GEM_BLACK, GEM_WHITE);
        /* battery bar */
        gfx_rect(gx + 8, gy + 22, 200, 12, GEM_BLACK);
        gfx_fill_rect(gx + 9, gy + 23, (200 - 2) * (bat < 0 ? 0 : bat > 100 ? 100 : bat) / 100, 10,
                      GEM_GREEN);
        snprintf(buf, sizeof buf, "CPU     : RP2350 @ 200 MHz dual core M33");
        gfx_puts_at(gx + 8, gy + 44, buf, GEM_BLACK, GEM_WHITE);
        snprintf(buf, sizeof buf, "RAM     : 520 KB SRAM");
        gfx_puts_at(gx + 8, gy + 58, buf, GEM_BLACK, GEM_WHITE);
        snprintf(buf, sizeof buf, "Display : 320x320 RGB666 @ SPI 62MHz");
        gfx_puts_at(gx + 8, gy + 72, buf, GEM_BLACK, GEM_WHITE);
        snprintf(buf, sizeof buf, "SD card : %s", os_sd_present ? "present" : "absent");
        gfx_puts_at(gx + 8, gy + 86, buf, GEM_BLACK, GEM_WHITE);
        uint32_t up = to_ms_since_boot(get_absolute_time()) / 1000;
        snprintf(buf, sizeof buf, "Uptime  : %02lu:%02lu:%02lu", up / 3600, (up / 60) % 60, up % 60);
        gfx_puts_at(gx + 8, gy + 100, buf, GEM_BLACK, GEM_WHITE);
        gfx_puts_at(gx, gy + gh - 10, "ESC=close", GEM_DGRAY, GEM_WHITE);
        gfx_flush();

        /* non-blocking key check with 500ms refresh */
        kbd_event_t ev;
        absolute_time_t until = make_timeout_time_ms(500);
        bool quit = false;
        while (!time_reached(until)) {
            kbd_poll(); sound_update();
            if (kbd_get_event(&ev) && ev.type == KBD_EV_PRESS && ev.code == KEY_ESC) { quit = true; break; }
            sleep_ms(10);
        }
        if (quit) return;
    }
}

/* ================= SETTINGS ================= */

void app_settings(void) {
    static uint8_t lcd_bl = 255, kbd_bl = 80, click = 1;
    int sel = 0;
    const char *items[] = {"LCD backlight", "KBD backlight", "Key click", "Power off"};
    for (;;) {
        draw_frame("SETTINGS");
        char buf[40];
        int y = gy + 8;
        for (int i = 0; i < 4; i++) {
            uint8_t fg = (i == sel) ? GEM_WHITE : GEM_BLACK;
            uint8_t bg = (i == sel) ? GEM_GREEN : GEM_WHITE;
            gfx_fill_rect(gx + 6, y, gw - 12, FONT_H, bg);
            if (i == 0) snprintf(buf, sizeof buf, "%-14s %3d", items[i], lcd_bl);
            else if (i == 1) snprintf(buf, sizeof buf, "%-14s %3d", items[i], kbd_bl);
            else if (i == 2) snprintf(buf, sizeof buf, "%-14s %s", items[i], click ? "on" : "off");
            else snprintf(buf, sizeof buf, "%s", items[i]);
            gfx_puts_at(gx + 10, y, buf, fg, bg);
            y += 14;
        }
        gfx_puts_at(gx + 6, gy + 80, "LEFT/RIGHT adjust, ENTER=act", GEM_DGRAY, GEM_WHITE);
        gfx_puts_at(gx, gy + gh - 10, "ESC=close", GEM_DGRAY, GEM_WHITE);
        gfx_flush();
        int c; wait_key(&c);
        sound_click();
        if (c == KEY_ESC) return;
        if (c == KEY_UP && sel > 0) sel--;
        if (c == KEY_DOWN && sel < 3) sel++;
        if (c == KEY_LEFT) {
            if (sel == 0 && lcd_bl >= 16) { lcd_bl -= 16; kbd_set_lcd_backlight(lcd_bl); }
            if (sel == 1 && kbd_bl >= 16) { kbd_bl -= 16; kbd_set_backlight(kbd_bl); }
        }
        if (c == KEY_RIGHT) {
            if (sel == 0 && lcd_bl <= 239) { lcd_bl += 16; kbd_set_lcd_backlight(lcd_bl); }
            if (sel == 1 && kbd_bl <= 239) { kbd_bl += 16; kbd_set_backlight(kbd_bl); }
        }
        if (c == KEY_ENTER) {
            if (sel == 2) click = !click;
            if (sel == 3) kbd_power_off();
        }
    }
}

/* ================= ABOUT ================= */

void app_about(void) {
    os_gem_desktop_bg();
    int cx, cy, cw, ch;
    os_window("ABOUT", &cx, &cy, &cw, &ch);
    const char *logo[] = {
        " ____  ____      _            ",
        "|  _ \\|  _ \\ ___| |_ _ __ ___ ",
        "| |_) | |_) / _ \\ __| '__/ _ \\",
        "|  __/|  _ <  __/ |_| | | (_)",
        "|_|   |_| \\_\\___|\\__|_|  \\___",
    };
    int y = cy + 16;
    for (int i = 0; i < 5; i++) { gfx_puts_at(cx + 16, y, logo[i], GEM_GREEN, GEM_WHITE); y += 8; }
    gfx_puts_at(cx + 60, y + 12, "P R e t r o C a l c   O S", GEM_BLACK, GEM_WHITE);
    gfx_puts_at(cx + 88, y + 24, "v1.0  for  PicoCalc", GEM_DGRAY, GEM_WHITE);
    gfx_puts_at(cx + 24, y + 44, "RP2350 * 320x320 * 520KB RAM * GEM/TOS", GEM_DGRAY, GEM_WHITE);
    gfx_puts_at(cx + 52, y + 60, "Written in C with the Pico SDK", GEM_DGRAY, GEM_WHITE);
    gfx_puts_at(cx + 30, y + 74, "PicoScript for user programs on SD card", GEM_DGRAY, GEM_WHITE);
    gfx_puts_at(cx + 90, y + 96, "Press any key", GEM_DGRAY, GEM_WHITE);
    gfx_flush();
    int c; wait_key(&c);
}
