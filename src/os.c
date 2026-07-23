/* PRetroCalc OS - core services: virtual terminal, status bar, line input */
#include "os.h"
#include "gfx.h"
#include "font.h"
#include "keyboard.h"
#include "sound.h"
#include "net.h"
#include "apps.h"
#include "board.h"
#include "pico/stdlib.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#define STATUS_H    10
#define TERM_TOP    STATUS_H
#define TERM_VIS_ROWS ((LCD_HEIGHT - TERM_TOP) / FONT_H)   /* 38 */
#define HIST_MAX    8
#define LINE_MAX    120

/* Terminal geometry: when windowed, text lives in the GEM window client area.
 * Term buffer is TERM_COLS x TERM_ROWS (40x40); only the visible portion is
 * drawn. Term uses a scrollback so long output never "goes off screen". */
#define TERM_VIS_COLS 36
#define TERM_WIN_VIS_ROWS 33

bool os_sd_present = false;

static char screen[TERM_ROWS][TERM_COLS];
static int cur_col = 0, cur_row = 0;      /* cursor in screen coords */
static uint8_t cur_fg = COL_LGREEN, cur_bg = COL_BLACK;

/* terminal render origin + windowed flag (windowed = draw into GEM window) */
static int term_ox = 0, term_oy = TERM_TOP;
static bool term_windowed = false;

static int term_cols(void) { return term_windowed ? TERM_VIS_COLS : TERM_COLS; }
static int term_rows(void) { return term_windowed ? TERM_WIN_VIS_ROWS : TERM_VIS_ROWS; }

static char history[HIST_MAX][LINE_MAX];
static int hist_count = 0;

static void scroll(void) {
    memmove(screen, screen[1], (TERM_ROWS - 1) * TERM_COLS);
    memset(screen[TERM_ROWS - 1], ' ', TERM_COLS);
    if (cur_row > 0) cur_row--;
}

void os_term_init(void) {
    memset(screen, ' ', sizeof(screen));
    cur_col = 0; cur_row = 0;
}

void os_set_color(uint8_t fg, uint8_t bg) { cur_fg = fg; cur_bg = bg; }

/* forward decls of geometry helpers defined below */
static int term_cols(void);
static int term_rows(void);

void os_putchar(char c) {
    int cols = term_cols(), rows = term_rows();
    if (c == '\n') {
        cur_col = 0;
        if (++cur_row >= rows) scroll();
        return;
    }
    if (c == '\r') { cur_col = 0; return; }
    if (c == '\b') { if (cur_col > 0) screen[cur_row][--cur_col] = ' '; return; }
    screen[cur_row][cur_col] = c;
    if (++cur_col >= cols) { cur_col = 0; if (++cur_row >= rows) scroll(); }
}

void os_print(const char *s) { while (*s) os_putchar(*s++); }

void os_printf(const char *fmt, ...) {
    char buf[256];
    va_list ap; va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    os_print(buf);
}

void os_clear_screen(void) {
    os_term_init();
    if (term_windowed) {
        gfx_fill_rect(term_ox, term_oy, term_cols() * FONT_W, term_rows() * FONT_H, GEM_WHITE);
    } else {
        gfx_fill_rect(0, TERM_TOP, LCD_WIDTH, LCD_HEIGHT - TERM_TOP, COL_BLACK);
    }
}

void os_draw_status_bar(void) {
    gfx_fill_rect(0, 0, LCD_WIDTH, STATUS_H, COL_DKBLUE);
    int bat = kbd_battery_percent();
    char buf[48];
    snprintf(buf, sizeof(buf), "PRetroCalc OS v1.0  BAT:%3d%% %s", bat < 0 ? 0 : bat,
             os_sd_present ? "SD" : "  ");
    gfx_puts_at(2, 1, buf, COL_WHITE, COL_DKBLUE);
    gfx_hline(0, STATUS_H - 1, LCD_WIDTH, COL_CYAN);
}

/* call after drawing a GEM window; text will be placed inside it */
void os_term_attach_window(void) {
    int cx, cy, cw, ch;
    os_window("TERMINAL", &cx, &cy, &cw, &ch);
    term_ox = cx; term_oy = cy;
    term_windowed = true;
}

void os_term_fullscreen(void) {
    term_ox = 0; term_oy = TERM_TOP;
    term_windowed = false;
}

/* repaint the terminal text region */
void os_render_term(void) {
    int cols = term_cols(), rows = term_rows();
    int ox = term_ox, oy = term_oy;
    uint8_t bg = term_windowed ? GEM_WHITE : cur_bg;
    uint8_t fg = term_windowed ? GEM_BLACK : cur_fg;
    gfx_fill_rect(ox, oy, cols * FONT_W, rows * FONT_H, bg);
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < cols; c++) {
            char ch = screen[r][c];
            if (ch != ' ') gfx_glyph(ox + c * FONT_W, oy + r * FONT_H, ch, fg, bg);
        }
    /* cursor */
    gfx_fill_rect(ox + cur_col * FONT_W, oy + cur_row * FONT_H + FONT_H - 1, FONT_W, 1, fg);
}

static void hist_push(const char *line) {
    if (line[0] == '\0') return;
    if (hist_count > 0 && strcmp(history[hist_count - 1], line) == 0) return;
    if (hist_count == HIST_MAX) {
        memmove(history, history + 1, (HIST_MAX - 1) * LINE_MAX);
        hist_count--;
    }
    strncpy(history[hist_count], line, LINE_MAX - 1);
    history[hist_count][LINE_MAX - 1] = 0;
    hist_count++;
}

/* ---------- GEM/TOS window chrome ---------- */

#define WIN_BORDER 2
#define WIN_TITLE_H 14
#define WIN_MARGIN 8

void os_gem_desktop_bg(void) {
    /* GEM desktop: white with subtle hatched pattern feel via light gray fill */
    gfx_clear(GEM_LGRAY);
}

void os_gem_menubar(const char *left, const char *right) {
    gfx_fill_rect(0, 0, LCD_WIDTH, 12, GEM_WHITE);
    gfx_hline(0, 11, LCD_WIDTH, GEM_BLACK);
    gfx_puts_at(4, 2, left, GEM_BLACK, GEM_WHITE);
    int rw = strlen(right) * 8;
    gfx_puts_at(LCD_WIDTH - rw - 4, 2, right, GEM_BLACK, GEM_WHITE);
}

void os_window(const char *title, int *cx, int *cy, int *cw, int *ch) {
    /* drop shadow (offset down-right) */
    gfx_fill_rect(WIN_MARGIN + 3, WIN_MARGIN + 3, LCD_WIDTH - 2 * WIN_MARGIN, LCD_HEIGHT - 2 * WIN_MARGIN, GEM_DGRAY);
    int wx = WIN_MARGIN, wy = WIN_MARGIN;
    int ww = LCD_WIDTH - 2 * WIN_MARGIN, wh = LCD_HEIGHT - 2 * WIN_MARGIN;
    gfx_fill_rect(wx, wy, ww, wh, GEM_WHITE);
    /* GEM 3D frame: black outer, white inner */
    gfx_rect(wx, wy, ww, wh, GEM_BLACK);
    gfx_rect(wx + 1, wy + 1, ww - 2, wh - 2, GEM_BLACK);
    /* title bar: white with top+bottom rules, close box on LEFT */
    gfx_fill_rect(wx + 2, wy + 2, ww - 4, WIN_TITLE_H, GEM_WHITE);
    gfx_hline(wx + 2, wy + 2 + WIN_TITLE_H, ww - 4, GEM_BLACK);
    /* close box (GEM: hollow square, top-left) */
    gfx_rect(wx + 5, wy + 5, 8, 8, GEM_BLACK);
    /* title centered with dashed underline look (GEM) */
    int tw = strlen(title) * 8;
    gfx_puts_at(wx + (ww - tw) / 2, wy + 5, title, GEM_BLACK, GEM_WHITE);
    /* right-side "fuller" box (GEM has an outlined square at right) */
    gfx_rect(wx + ww - 13, wy + 5, 8, 8, GEM_BLACK);
    /* client area */
    if (cx) *cx = wx + WIN_BORDER + 2;
    if (cy) *cy = wy + 2 + WIN_TITLE_H + 3;
    if (cw) *cw = ww - 2 * WIN_BORDER - 4;
    if (ch) *ch = wh - WIN_TITLE_H - 2 * WIN_BORDER - 5;
}

void os_window_close_box(void) { /* placeholder for future feedback */ }

/* blocking line editor; echo via terminal, redraw each keystroke */
int os_read_line(char *buf, int maxlen) {
    int len = 0;
    int hist_sel = hist_count;
    buf[0] = 0;
    for (;;) {
        os_render_term();
        gfx_flush();
        kbd_event_t ev;
        for (;;) {
            kbd_poll();
            sound_update();
            net_poll();
            if (kbd_get_event(&ev)) break;
            sleep_ms(4);
        }
        if (ev.type != KBD_EV_PRESS) continue;
        uint8_t c = ev.code;
        sound_click();
        if (c == KEY_ENTER) {
            os_putchar('\n');
            hist_push(buf);
            return len;
        }
        if (c == KEY_ESC) { buf[0] = 0; return -1; }
        if (c == KEY_BACKSPACE) {
            if (len > 0) { buf[--len] = 0; os_putchar('\b'); }
            continue;
        }
        if (c == KEY_UP) {
            if (hist_sel > 0) {
                hist_sel--;
                while (len--) os_putchar('\b');
                strncpy(buf, history[hist_sel], maxlen - 1);
                len = strlen(buf);
                os_print(buf);
            }
            continue;
        }
        if (c == KEY_DOWN) {
            if (hist_sel < hist_count) {
                hist_sel++;
                while (len--) os_putchar('\b');
                if (hist_sel < hist_count) { strncpy(buf, history[hist_sel], maxlen - 1); len = strlen(buf); }
                else { buf[0] = 0; len = 0; }
                os_print(buf);
            }
            continue;
        }
        if (c >= 32 && c < 127 && len < maxlen - 1) {
            buf[len++] = c; buf[len] = 0;
            os_putchar(c);
        }
    }
}
