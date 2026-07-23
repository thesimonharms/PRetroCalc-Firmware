#ifndef GFX_H
#define GFX_H

#include <stdint.h>
#include <stdbool.h>
#include "board.h"

#define FONT_W 8
#define FONT_H 8
#define TERM_COLS (LCD_WIDTH / FONT_W)    /* 40 */
#define TERM_ROWS (LCD_HEIGHT / FONT_H)   /* 40 */

/* RGB332 color helpers */
#define RGB332(r,g,b) ((((r) >> 5) << 5) | (((g) >> 5) << 2) | ((b) >> 6))
#define COL_BLACK   0x00
#define COL_WHITE   0xFF
#define COL_RED     0xE0
#define COL_GREEN   0x1C
#define COL_BLUE    0x03
#define COL_YELLOW  0xFC
#define COL_CYAN    0x1F
#define COL_MAGENTA 0xE3
#define COL_ORANGE  0xE8
#define COL_GRAY    0x92
#define COL_DKGRAY  0x49
#define COL_AMBER   0xF4
#define COL_LGREEN  0x3C
#define COL_DKBLUE  0x02
#define COL_DKGREEN 0x08

extern uint8_t gfx_fb[LCD_WIDTH * LCD_HEIGHT];

void gfx_init(void);
void gfx_pixel(int x, int y, uint8_t c);
void gfx_clear(uint8_t c);
void gfx_fill_rect(int x, int y, int w, int h, uint8_t c);
void gfx_rect(int x, int y, int w, int h, uint8_t c);
void gfx_hline(int x, int y, int w, uint8_t c);
void gfx_vline(int x, int y, int h, uint8_t c);
void gfx_line(int x0, int y0, int x1, int y1, uint8_t c);
void gfx_circle(int cx, int cy, int r, uint8_t c);
void gfx_fill_circle(int cx, int cy, int r, uint8_t c);
void gfx_blit(int x, int y, int w, int h, const uint8_t *data); /* 0xFF = transparent */

void gfx_set_cursor(int x, int y);
int  gfx_cursor_x(void);
int  gfx_cursor_y(void);
void gfx_set_color(uint8_t fg, uint8_t bg);
uint8_t gfx_fg(void);
void gfx_glyph(int x, int y, char ch, uint8_t fg, uint8_t bg);
void gfx_char(char ch, uint8_t fg, uint8_t bg);
void gfx_puts_at(int x, int y, const char *s, uint8_t fg, uint8_t bg);
void gfx_print(const char *s);
void gfx_print_n(const char *s, int n);

void gfx_flush(void);        /* push dirty rect */
void gfx_flush_full(void);
void gfx_scroll_up(int px, uint8_t fill);
void gfx_scroll_region_up(int x, int y, int w, int h, int px, uint8_t fill);

#endif
