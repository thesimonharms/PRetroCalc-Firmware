/* PRetroCalc OS - ILI9488/ST7365P graphics driver, 320x320 RGB666 over SPI1.
 * Framebuffer lives in SRAM (96KB, RGB332 1 byte/pixel), flushed to the LCD
 * line-by-line through DMA with 332->666 expansion. Optimized writes use
 * 32-bit SPI FIFO packing. */
#include "gfx.h"
#include "font.h"
#include "board.h"
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include <string.h>
#include <stdlib.h>

uint8_t gfx_fb[LCD_WIDTH * LCD_HEIGHT];   /* RGB332 draw framebuffer */
static uint8_t gfx_disp[LCD_WIDTH * LCD_HEIGHT]; /* displayed frame (RP2350 has RAM to spare) */

static uint8_t linebuf[LCD_WIDTH * 3] __attribute__((aligned(4)));
static int dma_chan = -1;
static uint8_t cursor_fg = COL_WHITE, cursor_bg = COL_BLACK;

/* dirty region tracking */
static int dirty_x0 = LCD_WIDTH, dirty_y0 = LCD_HEIGHT, dirty_x1 = -1, dirty_y1 = -1;

static void mark_dirty(int x, int y) {
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= LCD_WIDTH) x = LCD_WIDTH - 1;
    if (y >= LCD_HEIGHT) y = LCD_HEIGHT - 1;
    if (x < dirty_x0) dirty_x0 = x;
    if (x > dirty_x1) dirty_x1 = x;
    if (y < dirty_y0) dirty_y0 = y;
    if (y > dirty_y1) dirty_y1 = y;
}

/* ---------- low-level SPI ---------- */

static void __not_in_flash_func(spi_write_fast)(const uint8_t *src, size_t len) {
    /* pack 4 bytes per FIFO write to keep the PL022 TX FIFO saturated */
    while (len >= 4) {
        while (!spi_is_writable(LCD_SPI_MOD)) tight_loop_contents();
        uint32_t w = (uint32_t)src[0] | ((uint32_t)src[1] << 8) |
                     ((uint32_t)src[2] << 16) | ((uint32_t)src[3] << 24);
        spi_get_hw(LCD_SPI_MOD)->dr = w;
        src += 4; len -= 4;
    }
    while (len--) {
        while (!spi_is_writable(LCD_SPI_MOD)) tight_loop_contents();
        spi_get_hw(LCD_SPI_MOD)->dr = *src++;
    }
}

static void spi_cmd(uint8_t c) {
    gpio_put(LCD_PIN_DC, 0);
    gpio_put(LCD_PIN_CS, 0);
    spi_write_blocking(LCD_SPI_MOD, &c, 1);
    gpio_put(LCD_PIN_CS, 1);
}

static void spi_data(uint8_t d) {
    gpio_put(LCD_PIN_DC, 1);
    gpio_put(LCD_PIN_CS, 0);
    spi_write_blocking(LCD_SPI_MOD, &d, 1);
    gpio_put(LCD_PIN_CS, 1);
}

static void set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    uint8_t buf[4];
    gpio_put(LCD_PIN_CS, 0);
    gpio_put(LCD_PIN_DC, 0);
    spi_write_blocking(LCD_SPI_MOD, (uint8_t[]){0x2A}, 1);
    gpio_put(LCD_PIN_DC, 1);
    buf[0] = x0 >> 8; buf[1] = x0; buf[2] = x1 >> 8; buf[3] = x1;
    spi_write_blocking(LCD_SPI_MOD, buf, 4);
    gpio_put(LCD_PIN_DC, 0);
    spi_write_blocking(LCD_SPI_MOD, (uint8_t[]){0x2B}, 1);
    gpio_put(LCD_PIN_DC, 1);
    buf[0] = y0 >> 8; buf[1] = y0; buf[2] = y1 >> 8; buf[3] = y1;
    spi_write_blocking(LCD_SPI_MOD, buf, 4);
    gpio_put(LCD_PIN_DC, 0);
    spi_write_blocking(LCD_SPI_MOD, (uint8_t[]){0x2C}, 1);
    gpio_put(LCD_PIN_DC, 1);
    /* leave CS low + DC=data for pixel stream */
}

void gfx_init(void) {
    gpio_init(LCD_PIN_CS);  gpio_set_dir(LCD_PIN_CS, GPIO_OUT);  gpio_put(LCD_PIN_CS, 1);
    gpio_init(LCD_PIN_DC);  gpio_set_dir(LCD_PIN_DC, GPIO_OUT);
    gpio_init(LCD_PIN_RST); gpio_set_dir(LCD_PIN_RST, GPIO_OUT);

    spi_init(LCD_SPI_MOD, LCD_SPI_SPEED);
    gpio_set_function(LCD_PIN_SCK, GPIO_FUNC_SPI);
    gpio_set_function(LCD_PIN_TX, GPIO_FUNC_SPI);
    gpio_set_function(LCD_PIN_RX, GPIO_FUNC_SPI);

    dma_chan = dma_claim_unused_channel(true);

    /* reset */
    gpio_put(LCD_PIN_RST, 1); sleep_ms(10);
    gpio_put(LCD_PIN_RST, 0); sleep_ms(10);
    gpio_put(LCD_PIN_RST, 1); sleep_ms(200);

    static const uint8_t gamma_p[15] = {0x00,0x03,0x09,0x08,0x16,0x0A,0x3F,0x78,0x4C,0x09,0x0A,0x08,0x16,0x1A,0x0F};
    static const uint8_t gamma_n[15] = {0x00,0x16,0x19,0x03,0x0F,0x05,0x32,0x45,0x46,0x04,0x0E,0x0D,0x35,0x37,0x0F};
    spi_cmd(0xE0); for (int i = 0; i < 15; i++) spi_data(gamma_p[i]);
    spi_cmd(0xE1); for (int i = 0; i < 15; i++) spi_data(gamma_n[i]);
    spi_cmd(0xC0); spi_data(0x17); spi_data(0x15);
    spi_cmd(0xC1); spi_data(0x41);
    spi_cmd(0xC5); spi_data(0x00); spi_data(0x12); spi_data(0x80);
    spi_cmd(0x36); spi_data(0x48);       /* MADCTL: MX | BGR (landscape 320x320) */
    spi_cmd(0x3A); spi_data(0x66);       /* 18-bit RGB666 */
    spi_cmd(0xB0); spi_data(0x00);
    spi_cmd(0xB1); spi_data(0xA0);       /* ~60Hz */
    spi_cmd(0x21);                        /* inversion on */
    spi_cmd(0xB4); spi_data(0x02);
    spi_cmd(0xB6); spi_data(0x02); spi_data(0x02); spi_data(0x3B);
    spi_cmd(0xB7); spi_data(0xC6);
    spi_cmd(0xE9); spi_data(0x00);
    spi_cmd(0xF7); spi_data(0xA9); spi_data(0x51); spi_data(0x2C); spi_data(0x82);
    spi_cmd(0x11); sleep_ms(120);
    spi_cmd(0x29); sleep_ms(20);

    gfx_clear(COL_BLACK);
}

/* ---------- pixel ops ---------- */

void gfx_pixel(int x, int y, uint8_t c) {
    if ((unsigned)x >= LCD_WIDTH || (unsigned)y >= LCD_HEIGHT) return;
    gfx_fb[y * LCD_WIDTH + x] = c;
    mark_dirty(x, y);
}

void gfx_clear(uint8_t c) {
    memset(gfx_fb, c, sizeof(gfx_fb));
    dirty_x0 = 0; dirty_y0 = 0; dirty_x1 = LCD_WIDTH-1; dirty_y1 = LCD_HEIGHT-1;
}

void gfx_fill_rect(int x, int y, int w, int h, uint8_t c) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > LCD_WIDTH)  w = LCD_WIDTH - x;
    if (y + h > LCD_HEIGHT) h = LCD_HEIGHT - y;
    if (w <= 0 || h <= 0) return;
    for (int r = 0; r < h; r++)
        memset(&gfx_fb[(y + r) * LCD_WIDTH + x], c, w);
    if (x < dirty_x0) dirty_x0 = x;
    if (x + w - 1 > dirty_x1) dirty_x1 = x + w - 1;
    if (y < dirty_y0) dirty_y0 = y;
    if (y + h - 1 > dirty_y1) dirty_y1 = y + h - 1;
}

void gfx_rect(int x, int y, int w, int h, uint8_t c) {
    gfx_fill_rect(x, y, w, 1, c);
    gfx_fill_rect(x, y + h - 1, w, 1, c);
    gfx_fill_rect(x, y, 1, h, c);
    gfx_fill_rect(x + w - 1, y, 1, h, c);
}

void gfx_hline(int x, int y, int w, uint8_t c) { gfx_fill_rect(x, y, w, 1, c); }
void gfx_vline(int x, int y, int h, uint8_t c) { gfx_fill_rect(x, y, 1, h, c); }

void gfx_line(int x0, int y0, int x1, int y1, uint8_t c) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        gfx_pixel(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void gfx_circle(int cx, int cy, int r, uint8_t c) {
    int x = -r, y = 0, err = 2 - 2 * r;
    do {
        gfx_pixel(cx - x, cy + y, c); gfx_pixel(cx - y, cy - x, c);
        gfx_pixel(cx + x, cy - y, c); gfx_pixel(cx + y, cy + x, c);
        r = err;
        if (r <= y) err += ++y * 2 + 1;
        if (r > x || err > y) err += ++x * 2 + 1;
    } while (x < 0);
}

void gfx_fill_circle(int cx, int cy, int r, uint8_t c) {
    for (int y = -r; y <= r; y++) {
        int dx = 0;
        while (dx * dx + y * y <= r * r) dx++;
        gfx_fill_rect(cx - dx + 1, cy + y, 2 * dx - 1, 1, c);
    }
}

/* ---------- text ---------- */

static int cursor_x = 0, cursor_y = 0;

void gfx_set_cursor(int x, int y) { cursor_x = x; cursor_y = y; }
int  gfx_cursor_x(void) { return cursor_x; }
int  gfx_cursor_y(void) { return cursor_y; }
void gfx_set_color(uint8_t fg, uint8_t bg) { cursor_fg = fg; cursor_bg = bg; }
uint8_t gfx_fg(void) { return cursor_fg; }

void gfx_glyph_bmp(int x, int y, const uint8_t *rows, uint8_t fg, uint8_t bg) {
    gfx_glyph_bmp_ex(x, y, rows, fg, bg, false, false);
}

void gfx_glyph_bmp_ex(int x, int y, const uint8_t *rows, uint8_t fg, uint8_t bg,
                      bool bold, bool italic) {
    if (!rows || x < 0 || y < 0 || x > LCD_WIDTH - FONT_W || y > LCD_HEIGHT - FONT_H) return;
    uint8_t *dst = &gfx_fb[y * LCD_WIDTH + x];
    for (int row = 0; row < FONT_H; row++) {
        uint8_t bits = rows[row];
        if (bold) bits |= (uint8_t)(bits << 1);
        int shift = italic ? (row / 3) : 0;
        for (int col = 0; col < FONT_W; col++) {
            int src = col - shift;
            uint8_t on = 0;
            if (src >= 0 && src < 8) on = (bits >> src) & 1;
            dst[col] = on ? fg : bg;
        }
        dst += LCD_WIDTH;
    }
    mark_dirty(x, y); mark_dirty(x + FONT_W - 1, y + FONT_H - 1);
}

void gfx_glyph_bmp_tr(int x, int y, const uint8_t *rows, uint8_t fg) {
    if (!rows || x < 0 || y < 0 || x > LCD_WIDTH - FONT_W || y > LCD_HEIGHT - FONT_H) return;
    uint8_t *dst = &gfx_fb[y * LCD_WIDTH + x];
    for (int row = 0; row < FONT_H; row++) {
        uint8_t bits = rows[row];
        for (int col = 0; col < FONT_W; col++) {
            if ((bits >> col) & 1) dst[col] = fg;
        }
        dst += LCD_WIDTH;
    }
    mark_dirty(x, y); mark_dirty(x + FONT_W - 1, y + FONT_H - 1);
}

void gfx_glyph_n(int x, int y, int w, int h, int row_bytes,
                 const uint8_t *bits, uint8_t fg, uint8_t bg, bool bold) {
    if (!bits || w <= 0 || h <= 0) return;
    if (x >= LCD_WIDTH || y >= LCD_HEIGHT) return;
    if (x + w <= 0 || y + h <= 0) return;
    bool tr = (bg == 0xFF);
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w; if (x1 > LCD_WIDTH) x1 = LCD_WIDTH;
    int y1 = y + h; if (y1 > LCD_HEIGHT) y1 = LCD_HEIGHT;
    for (int row = 0; row < h; row++) {
        int yy = y + row;
        if (yy < y0 || yy >= y1) continue;
        const uint8_t *rp = bits + row * row_bytes;
        uint8_t *dst = &gfx_fb[yy * LCD_WIDTH];
        for (int col = 0; col < w; col++) {
            int xx = x + col;
            if (xx < x0 || xx >= x1) continue;
            int bi = col >> 3;
            int bp = col & 7;
            uint8_t on = 0;
            if (bi < row_bytes) on = (rp[bi] >> bp) & 1;
            if (bold && col + 1 < w) {
                int bi2 = (col + 1) >> 3, bp2 = (col + 1) & 7;
                if (bi2 < row_bytes) on |= (rp[bi2] >> bp2) & 1;
            }
            if (on) dst[xx] = fg;
            else if (!tr) dst[xx] = bg;
        }
    }
    mark_dirty(x0, y0);
    mark_dirty(x1 - 1, y1 - 1);
}

void gfx_glyph_scale(int x, int y, char ch, int scale, uint8_t fg, uint8_t bg,
                     bool bold, bool italic) {
    if (scale < 1) scale = 1;
    bool tr = (bg == 0xFF);
    const uint8_t *g = &font8x8[(uint8_t)ch * 8];
    for (int row = 0; row < FONT_H; row++) {
        uint8_t bits = g[row];
        if (bold) bits |= (uint8_t)(bits << 1);
        int shift = italic ? ((FONT_H - 1 - row) / 3) : 0;
        for (int col = 0; col < FONT_W; col++) {
            int src = col - shift;
            uint8_t on = (src >= 0 && src < FONT_W) ? (uint8_t)((bits >> src) & 1) : 0;
            int px = x + col * scale;
            int py = y + row * scale;
            for (int dy = 0; dy < scale; dy++) {
                int yy = py + dy;
                if (yy < 0 || yy >= LCD_HEIGHT) continue;
                for (int dx = 0; dx < scale; dx++) {
                    int xx = px + dx;
                    if (xx < 0 || xx >= LCD_WIDTH) continue;
                    if (on) gfx_fb[yy * LCD_WIDTH + xx] = fg;
                    else if (!tr) gfx_fb[yy * LCD_WIDTH + xx] = bg;
                }
            }
        }
    }
    mark_dirty(x, y);
    int x1 = x + FONT_W * scale - 1, y1 = y + FONT_H * scale - 1;
    if (x1 >= LCD_WIDTH) x1 = LCD_WIDTH - 1;
    if (y1 >= LCD_HEIGHT) y1 = LCD_HEIGHT - 1;
    mark_dirty(x1, y1);
}

void gfx_glyph(int x, int y, char ch, uint8_t fg, uint8_t bg) {
    gfx_glyph_bmp(x, y, &font8x8[(uint8_t)ch * 8], fg, bg);
}

void gfx_char(char ch, uint8_t fg, uint8_t bg) {
    gfx_glyph(cursor_x, cursor_y, ch, fg, bg);
    cursor_x += FONT_W;
    if (cursor_x > LCD_WIDTH - FONT_W) { cursor_x = 0; cursor_y += FONT_H; }
}

void gfx_puts_at(int x, int y, const char *s, uint8_t fg, uint8_t bg) {
    if (y < 0 || y > LCD_HEIGHT - FONT_H) return;
    while (*s && x <= LCD_WIDTH - FONT_W) {
        if (x >= 0) gfx_glyph(x, y, *s, fg, bg);
        s++;
        x += FONT_W;
    }
}

void gfx_puts_fit(int x, int y, const char *s, uint8_t fg, uint8_t bg, int max_w) {
    if (!s || max_w < FONT_W || y < 0 || y > LCD_HEIGHT - FONT_H) return;
    int max_c = max_w / FONT_W;
    int len = (int)strlen(s);
    if (len <= max_c) {
        gfx_puts_at(x, y, s, fg, bg);
        return;
    }
    if (max_c <= 3) {
        for (int i = 0; i < max_c; i++) gfx_glyph(x + i * FONT_W, y, s[i], fg, bg);
        return;
    }
    int keep = max_c - 3;
    for (int i = 0; i < keep; i++) gfx_glyph(x + i * FONT_W, y, s[i], fg, bg);
    gfx_puts_at(x + keep * FONT_W, y, "...", fg, bg);
}

void gfx_print(const char *s) {
    while (*s) {
        char c = *s++;
        if (c == '\n') { cursor_x = 0; cursor_y += FONT_H; continue; }
        gfx_char(c, cursor_fg, cursor_bg);
    }
}

void gfx_print_n(const char *s, int n) {
    while (n-- && *s) gfx_char(*s++, cursor_fg, cursor_bg);
}

/* ---------- sprites / bitmap ---------- */

void gfx_blit(int x, int y, int w, int h, const uint8_t *data) {
    for (int r = 0; r < h; r++) {
        int dy = y + r;
        if ((unsigned)dy >= LCD_HEIGHT) continue;
        for (int c = 0; c < w; c++) {
            int dx = x + c;
            if ((unsigned)dx >= LCD_WIDTH) continue;
            uint8_t p = data[r * w + c];
            if (p != 0xFF) gfx_fb[dy * LCD_WIDTH + dx] = p; /* 0xFF = transparent */
        }
    }
    mark_dirty(x, y); mark_dirty(x + w - 1, y + h - 1);
}

/* ---------- flush ---------- */

static inline void expand_line(const uint8_t *src, uint8_t *dst, int n) {
    for (int i = 0; i < n; i++) {
        uint8_t p = src[i];
        /* RGB332 -> RGB666, each channel scaled to full 6-bit range then placed
         * in the top 6 bits of the byte (ILI9488 RGB666 uses bits 7..2).
         * The old code left blue capped at 192 -> white looked pink. */
        uint8_t r3 = (p >> 5) & 0x07;             /* 0..7 */
        uint8_t g3 = (p >> 2) & 0x07;             /* 0..7 */
        uint8_t b2 = p & 0x03;                    /* 0..3 */
        uint8_t r6 = (uint8_t)((r3 << 3) | r3);   /* 0..63 */
        uint8_t g6 = (uint8_t)((g3 << 3) | g3);   /* 0..63 */
        uint8_t b6 = (uint8_t)((b2 << 4) | (b2 << 2) | b2); /* 0..63 */
        *dst++ = (uint8_t)(r6 << 2);
        *dst++ = (uint8_t)(g6 << 2);
        *dst++ = (uint8_t)(b6 << 2);
    }
}

static void __not_in_flash_func(push_rect)(int x0, int y0, int x1, int y1) {
    set_window(x0, y0, x1, y1);
    int w = x1 - x0 + 1;
    for (int y = y0; y <= y1; y++) {
        expand_line(&gfx_disp[y * LCD_WIDTH + x0], linebuf, w);
        if (dma_channel_is_busy(dma_chan)) dma_channel_wait_for_finish_blocking(dma_chan);
        dma_channel_config cfg = dma_channel_get_default_config(dma_chan);
        channel_config_set_transfer_data_size(&cfg, DMA_SIZE_8);
        channel_config_set_dreq(&cfg, spi_get_dreq(LCD_SPI_MOD, true));
        dma_channel_configure(dma_chan, &cfg, &spi_get_hw(LCD_SPI_MOD)->dr,
                              linebuf, w * 3, true);
    }
    dma_channel_wait_for_finish_blocking(dma_chan);
    while (spi_get_hw(LCD_SPI_MOD)->sr & SPI_SSPSR_BSY_BITS) tight_loop_contents();
    gpio_put(LCD_PIN_CS, 1);
}

void gfx_flush(void) {
    if (dirty_x1 < 0) return;
    /* Clamp dirty rect to framebuffer — hscroll can dirty partial glyphs. */
    if (dirty_x0 < 0) dirty_x0 = 0;
    if (dirty_y0 < 0) dirty_y0 = 0;
    if (dirty_x1 >= LCD_WIDTH) dirty_x1 = LCD_WIDTH - 1;
    if (dirty_y1 >= LCD_HEIGHT) dirty_y1 = LCD_HEIGHT - 1;
    if (dirty_x1 < dirty_x0 || dirty_y1 < dirty_y0) {
        dirty_x0 = LCD_WIDTH; dirty_y0 = LCD_HEIGHT; dirty_x1 = -1; dirty_y1 = -1;
        return;
    }
    int w = dirty_x1 - dirty_x0 + 1;
    for (int y = dirty_y0; y <= dirty_y1; y++)
        memcpy(&gfx_disp[y * LCD_WIDTH + dirty_x0],
               &gfx_fb[y * LCD_WIDTH + dirty_x0], w);
    push_rect(dirty_x0, dirty_y0, dirty_x1, dirty_y1);
    dirty_x0 = LCD_WIDTH; dirty_y0 = LCD_HEIGHT; dirty_x1 = -1; dirty_y1 = -1;
}

void gfx_flush_full(void) {
    dirty_x0 = 0; dirty_y0 = 0; dirty_x1 = LCD_WIDTH - 1; dirty_y1 = LCD_HEIGHT - 1;
    gfx_flush();
}

/* vertical scroll of the whole framebuffer by n pixels (for terminal) */
void gfx_scroll_up(int px, uint8_t fill) {
    if (px <= 0) return;
    if (px >= LCD_HEIGHT) {
        memset(gfx_fb, fill, sizeof(gfx_fb));
    } else {
        memmove(gfx_fb, gfx_fb + px * LCD_WIDTH, (LCD_HEIGHT - px) * LCD_WIDTH);
        memset(gfx_fb + (LCD_HEIGHT - px) * LCD_WIDTH, fill, px * LCD_WIDTH);
    }
    dirty_x0 = 0; dirty_y0 = 0; dirty_x1 = LCD_WIDTH - 1; dirty_y1 = LCD_HEIGHT - 1;
}

/* vertical scroll of a rectangular region of the framebuffer upward by px
 * pixels (the top px rows are discarded, the bottom px rows are filled with
 * the fill colour). Used to keep scrolling text inside a window without
 * clobbering the rest of the screen. */
void gfx_scroll_region_up(int x, int y, int w, int h, int px, uint8_t fill) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > LCD_WIDTH) w = LCD_WIDTH - x;
    if (y + h > LCD_HEIGHT) h = LCD_HEIGHT - y;
    if (w <= 0 || h <= 0) return;
    if (px < 0) px = 0;
    if (px > h) px = h;
    if (px > 0) {
        for (int r = 0; r < h - px; r++)
            memmove(&gfx_fb[(y + r) * LCD_WIDTH + x],
                    &gfx_fb[(y + r + px) * LCD_WIDTH + x],
                    (size_t)w);
    }
    for (int r = h - px; r < h; r++)
        memset(&gfx_fb[(y + r) * LCD_WIDTH + x], fill, (size_t)w);
    mark_dirty(x, y);
    mark_dirty(x + w - 1, y + h - 1);
}
