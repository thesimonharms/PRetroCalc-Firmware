#include "gfx.h"
#include "font.h"
#include "board.h"
#include "pico/stdlib.h"
#include <driver/spi_master.h>
#include <driver/gpio.h>
#include <esp_heap_caps.h>
#include <string.h>
#include <stdlib.h>

/* Allocated at gfx_init — keeps 96KB out of .bss so the image links. */
uint8_t *gfx_fb;
static spi_device_handle_t lcd;
static uint8_t linebuf[LCD_WIDTH * 3] __attribute__((aligned(4)));
static uint8_t cursor_fg = COL_WHITE, cursor_bg = COL_BLACK;
static int cursor_x, cursor_y;
static int dirty_x0 = LCD_WIDTH, dirty_y0 = LCD_HEIGHT, dirty_x1 = -1, dirty_y1 = -1;

static void mark_dirty(int x, int y) {
    if (x < 0) x = 0; if (y < 0) y = 0;
    if (x >= LCD_WIDTH) x = LCD_WIDTH - 1; if (y >= LCD_HEIGHT) y = LCD_HEIGHT - 1;
    if (x < dirty_x0) dirty_x0 = x; if (x > dirty_x1) dirty_x1 = x;
    if (y < dirty_y0) dirty_y0 = y; if (y > dirty_y1) dirty_y1 = y;
}
static void spi_bytes(const uint8_t *data, size_t len) {
    spi_transaction_t t; memset(&t, 0, sizeof t);
    t.length = (uint32_t)(len * 8); t.tx_buffer = data;
    spi_device_polling_transmit(lcd, &t);
}
static void spi_cmd(uint8_t c) { gpio_set_level(LCD_PIN_DC, 0); spi_bytes(&c, 1); }
static void spi_data(uint8_t d) { gpio_set_level(LCD_PIN_DC, 1); spi_bytes(&d, 1); }
static void set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    uint8_t b[4] = { (uint8_t)(x0 >> 8), (uint8_t)x0, (uint8_t)(x1 >> 8), (uint8_t)x1 };
    spi_cmd(0x2a); gpio_set_level(LCD_PIN_DC, 1); spi_bytes(b, sizeof b);
    b[0] = (uint8_t)(y0 >> 8); b[1] = (uint8_t)y0; b[2] = (uint8_t)(y1 >> 8); b[3] = (uint8_t)y1;
    spi_cmd(0x2b); gpio_set_level(LCD_PIN_DC, 1); spi_bytes(b, sizeof b);
    spi_cmd(0x2c); gpio_set_level(LCD_PIN_DC, 1);
}
static void expand_line(const uint8_t *src, uint8_t *dst, int n) {
    while (n--) {
        uint8_t p = *src++, r3 = p >> 5, g3 = (p >> 2) & 7, b2 = p & 3;
        *dst++ = (uint8_t)((r3 << 3 | r3) << 2);
        *dst++ = (uint8_t)((g3 << 3 | g3) << 2);
        *dst++ = (uint8_t)((b2 << 4 | b2 << 2 | b2) << 2);
    }
}

void gfx_init(void) {
    if (!gfx_fb) {
        gfx_fb = (uint8_t *)heap_caps_malloc(LCD_WIDTH * LCD_HEIGHT,
                                             MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!gfx_fb) return;
    }
    gpio_config_t io = { .pin_bit_mask = (1ULL << LCD_PIN_DC) | (1ULL << LCD_PIN_RST),
                          .mode = GPIO_MODE_OUTPUT, .pull_up_en = GPIO_PULLUP_DISABLE,
                          .pull_down_en = GPIO_PULLDOWN_DISABLE, .intr_type = GPIO_INTR_DISABLE };
    gpio_config(&io);
    spi_bus_config_t bus = { .mosi_io_num = LCD_PIN_TX, .miso_io_num = LCD_PIN_RX,
                             .sclk_io_num = LCD_PIN_SCK, .quadwp_io_num = -1, .quadhd_io_num = -1,
                             .max_transfer_sz = LCD_WIDTH * 3 };
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return;
    spi_device_interface_config_t dev = { .clock_speed_hz = LCD_SPI_SPEED, .mode = 0,
                                           .spics_io_num = LCD_PIN_CS, .queue_size = 1 };
    if (spi_bus_add_device(SPI2_HOST, &dev, &lcd) != ESP_OK) return;
    gpio_set_level(LCD_PIN_RST, 1); sleep_ms(10); gpio_set_level(LCD_PIN_RST, 0);
    sleep_ms(10); gpio_set_level(LCD_PIN_RST, 1); sleep_ms(200);
    static const uint8_t gp[] = {0,3,9,8,0x16,10,0x3f,0x78,0x4c,9,10,8,0x16,0x1a,15};
    static const uint8_t gn[] = {0,0x16,0x19,3,15,5,0x32,0x45,0x46,4,14,13,0x35,0x37,15};
    spi_cmd(0xe0); for (int i=0;i<15;i++) spi_data(gp[i]);
    spi_cmd(0xe1); for (int i=0;i<15;i++) spi_data(gn[i]);
    spi_cmd(0xc0); spi_data(0x17); spi_data(0x15); spi_cmd(0xc1); spi_data(0x41);
    spi_cmd(0xc5); spi_data(0); spi_data(0x12); spi_data(0x80);
    spi_cmd(0x36); spi_data(0x48); spi_cmd(0x3a); spi_data(0x66);
    spi_cmd(0xb0); spi_data(0); spi_cmd(0xb1); spi_data(0xa0); spi_cmd(0x21);
    spi_cmd(0xb4); spi_data(2); spi_cmd(0xb6); spi_data(2); spi_data(2); spi_data(0x3b);
    spi_cmd(0xb7); spi_data(0xc6); spi_cmd(0xe9); spi_data(0);
    spi_cmd(0xf7); spi_data(0xa9); spi_data(0x51); spi_data(0x2c); spi_data(0x82);
    spi_cmd(0x11); sleep_ms(120); spi_cmd(0x29); sleep_ms(20); gfx_clear(COL_BLACK);
}
void gfx_pixel(int x,int y,uint8_t c) { if (!gfx_fb) return; if ((unsigned)x<LCD_WIDTH && (unsigned)y<LCD_HEIGHT) { gfx_fb[y*LCD_WIDTH+x]=c; mark_dirty(x,y); } }
void gfx_clear(uint8_t c) { if (!gfx_fb) return; memset(gfx_fb,c,LCD_WIDTH*LCD_HEIGHT); dirty_x0=dirty_y0=0; dirty_x1=LCD_WIDTH-1; dirty_y1=LCD_HEIGHT-1; }
void gfx_fill_rect(int x,int y,int w,int h,uint8_t c) {
    if (!gfx_fb) return;
    if(x<0){w+=x;x=0;} if(y<0){h+=y;y=0;} if(x+w>LCD_WIDTH)w=LCD_WIDTH-x; if(y+h>LCD_HEIGHT)h=LCD_HEIGHT-y;
    if(w<=0||h<=0)return; for(int r=0;r<h;r++)memset(gfx_fb+(y+r)*LCD_WIDTH+x,c,w); mark_dirty(x,y);mark_dirty(x+w-1,y+h-1);
}
void gfx_rect(int x,int y,int w,int h,uint8_t c){gfx_fill_rect(x,y,w,1,c);gfx_fill_rect(x,y+h-1,w,1,c);gfx_fill_rect(x,y,1,h,c);gfx_fill_rect(x+w-1,y,1,h,c);}
void gfx_hline(int x,int y,int w,uint8_t c){gfx_fill_rect(x,y,w,1,c);} void gfx_vline(int x,int y,int h,uint8_t c){gfx_fill_rect(x,y,1,h,c);}
void gfx_line(int x0,int y0,int x1,int y1,uint8_t c){int dx=abs(x1-x0),sx=x0<x1?1:-1,dy=-abs(y1-y0),sy=y0<y1?1:-1,e=dx+dy;for(;;){gfx_pixel(x0,y0,c);if(x0==x1&&y0==y1)break;int e2=2*e;if(e2>=dy){e+=dy;x0+=sx;}if(e2<=dx){e+=dx;y0+=sy;}}}
void gfx_circle(int cx,int cy,int r,uint8_t c){int x=-r,y=0,e=2-2*r;do{gfx_pixel(cx-x,cy+y,c);gfx_pixel(cx-y,cy-x,c);gfx_pixel(cx+x,cy-y,c);gfx_pixel(cx+y,cy+x,c);r=e;if(r<=y)e+=++y*2+1;if(r>x||e>y)e+=++x*2+1;}while(x<0);}
void gfx_fill_circle(int cx,int cy,int r,uint8_t c){for(int y=-r;y<=r;y++){int dx=0;while(dx*dx+y*y<=r*r)dx++;gfx_fill_rect(cx-dx+1,cy+y,2*dx-1,1,c);}}
void gfx_set_cursor(int x,int y){cursor_x=x;cursor_y=y;} int gfx_cursor_x(void){return cursor_x;} int gfx_cursor_y(void){return cursor_y;}
void gfx_set_color(uint8_t fg,uint8_t bg){cursor_fg=fg;cursor_bg=bg;} uint8_t gfx_fg(void){return cursor_fg;}
void gfx_glyph_bmp_ex(int x,int y,const uint8_t *rows,uint8_t fg,uint8_t bg,bool bold,bool italic){if(!gfx_fb||!rows||x<0||y<0||x>LCD_WIDTH-FONT_W||y>LCD_HEIGHT-FONT_H)return;for(int r=0;r<8;r++){uint8_t bits=rows[r];if(bold)bits|=bits<<1;int sh=italic?r/3:0;for(int c=0;c<8;c++){int s=c-sh;gfx_fb[(y+r)*LCD_WIDTH+x+c]=(s>=0&&s<8&&((bits>>s)&1))?fg:bg;}}mark_dirty(x,y);mark_dirty(x+7,y+7);}
void gfx_glyph_bmp(int x,int y,const uint8_t *rows,uint8_t fg,uint8_t bg){gfx_glyph_bmp_ex(x,y,rows,fg,bg,false,false);}
void gfx_glyph_bmp_tr(int x,int y,const uint8_t *rows,uint8_t fg){if(!gfx_fb||!rows||x<0||y<0||x>LCD_WIDTH-8||y>LCD_HEIGHT-8)return;for(int r=0;r<8;r++)for(int c=0;c<8;c++)if((rows[r]>>c)&1)gfx_fb[(y+r)*LCD_WIDTH+x+c]=fg;mark_dirty(x,y);mark_dirty(x+7,y+7);}
void gfx_glyph_n(int x, int y, int w, int h, int row_bytes,
                 const uint8_t *bits, uint8_t fg, uint8_t bg, bool bold) {
    if (!gfx_fb || !bits || w <= 0 || h <= 0) return;
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
void gfx_glyph_scale(int x,int y,char ch,int sc,uint8_t fg,uint8_t bg,bool bold,bool italic){if(!gfx_fb)return;if(sc<1)sc=1;const uint8_t *g=font8x8+(uint8_t)ch*8;for(int r=0;r<8;r++){uint8_t b=g[r];if(bold)b|=b<<1;int sh=italic?(7-r)/3:0;for(int c=0;c<8;c++){int s=c-sh;bool on=s>=0&&s<8&&((b>>s)&1);if(on||bg!=0xff)gfx_fill_rect(x+c*sc,y+r*sc,sc,sc,on?fg:bg);}}}
void gfx_glyph(int x,int y,char ch,uint8_t fg,uint8_t bg){gfx_glyph_bmp(x,y,font8x8+(uint8_t)ch*8,fg,bg);}
void gfx_char(char c,uint8_t fg,uint8_t bg){gfx_glyph(cursor_x,cursor_y,c,fg,bg);cursor_x+=8;if(cursor_x>LCD_WIDTH-8){cursor_x=0;cursor_y+=8;}}
void gfx_puts_at(int x,int y,const char *s,uint8_t fg,uint8_t bg){if(y<0||y>LCD_HEIGHT-8)return;while(*s&&x<=LCD_WIDTH-8){if(x>=0)gfx_glyph(x,y,*s,fg,bg);s++;x+=8;}}
void gfx_print(const char*s){while(*s){char c=*s++;if(c=='\n'){cursor_x=0;cursor_y+=8;}else gfx_char(c,cursor_fg,cursor_bg);}} void gfx_print_n(const char*s,int n){while(n--&&*s)gfx_char(*s++,cursor_fg,cursor_bg);}
void gfx_blit(int x,int y,int w,int h,const uint8_t*d){if(!gfx_fb||!d)return;for(int r=0;r<h;r++)for(int c=0;c<w;c++){int xx=x+c,yy=y+r;if((unsigned)xx<LCD_WIDTH&&(unsigned)yy<LCD_HEIGHT&&d[r*w+c]!=0xff)gfx_fb[yy*LCD_WIDTH+xx]=d[r*w+c];}mark_dirty(x,y);mark_dirty(x+w-1,y+h-1);}
void gfx_flush(void){if(!gfx_fb||dirty_x1<0||!lcd)return;if(dirty_x0<0)dirty_x0=0;if(dirty_y0<0)dirty_y0=0;if(dirty_x1>=LCD_WIDTH)dirty_x1=LCD_WIDTH-1;if(dirty_y1>=LCD_HEIGHT)dirty_y1=LCD_HEIGHT-1;if(dirty_x1>=dirty_x0&&dirty_y1>=dirty_y0){int w=dirty_x1-dirty_x0+1;set_window(dirty_x0,dirty_y0,dirty_x1,dirty_y1);for(int y=dirty_y0;y<=dirty_y1;y++){expand_line(gfx_fb+y*LCD_WIDTH+dirty_x0,linebuf,w);spi_bytes(linebuf,w*3);}}dirty_x0=LCD_WIDTH;dirty_y0=LCD_HEIGHT;dirty_x1=dirty_y1=-1;}
void gfx_flush_full(void){dirty_x0=dirty_y0=0;dirty_x1=LCD_WIDTH-1;dirty_y1=LCD_HEIGHT-1;gfx_flush();}
void gfx_scroll_up(int px,uint8_t fill){if(!gfx_fb||px<=0)return;if(px>=LCD_HEIGHT)memset(gfx_fb,fill,LCD_WIDTH*LCD_HEIGHT);else{memmove(gfx_fb,gfx_fb+px*LCD_WIDTH,(LCD_HEIGHT-px)*LCD_WIDTH);memset(gfx_fb+(LCD_HEIGHT-px)*LCD_WIDTH,fill,px*LCD_WIDTH);}dirty_x0=dirty_y0=0;dirty_x1=LCD_WIDTH-1;dirty_y1=LCD_HEIGHT-1;}
void gfx_scroll_region_up(int x,int y,int w,int h,int px,uint8_t fill){if(!gfx_fb)return;if(x<0){w+=x;x=0;}if(y<0){h+=y;y=0;}if(x+w>LCD_WIDTH)w=LCD_WIDTH-x;if(y+h>LCD_HEIGHT)h=LCD_HEIGHT-y;if(w<=0||h<=0)return;if(px<0)px=0;if(px>h)px=h;for(int r=0;r<h-px;r++)memmove(gfx_fb+(y+r)*LCD_WIDTH+x,gfx_fb+(y+r+px)*LCD_WIDTH+x,w);for(int r=h-px;r<h;r++)memset(gfx_fb+(y+r)*LCD_WIDTH+x,fill,w);mark_dirty(x,y);mark_dirty(x+w-1,y+h-1);}
