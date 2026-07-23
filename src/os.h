#ifndef OS_H
#define OS_H

#include <stdint.h>
#include <stdbool.h>

/* GEM/TOS palette */
#define GEM_WHITE   0xFF
#define GEM_BLACK   0x00
#define GEM_GREEN   0x10   /* dark green accent */
#define GEM_LGRAY   0xB6
#define GEM_DGRAY   0x49
#define GEM_TITLEBG 0x10

/* Virtual-screen terminal: 40 cols x 40 rows of 8x8 glyphs. */
void os_term_init(void);
void os_putchar(char c);
void os_print(const char *s);
void os_printf(const char *fmt, ...);
void os_set_color(uint8_t fg, uint8_t bg);
void os_clear_screen(void);
void os_draw_status_bar(void);          /* redraw top status bar */
void os_render_term(void);              /* repaint whole terminal region */

/* GEM-style pseudo-window chrome: title bar + 3D border + white client area.
 * Returns client rect via out params (content starts below title bar). */
void os_window(const char *title, int *cx, int *cy, int *cw, int *ch);
void os_window_close_box(void);         /* redraws the close box highlight */

/* GEM desktop for the launcher */
void os_gem_desktop_bg(void);
void os_gem_menubar(const char *left, const char *right);

/* line input with editing + history; returns length, -1 on ESC-cancel */
int  os_read_line(char *buf, int maxlen);

/* app launch + return to launcher */
void os_launch_app(int idx);

/* SD availability (set at boot) */
extern bool os_sd_present;

#endif
