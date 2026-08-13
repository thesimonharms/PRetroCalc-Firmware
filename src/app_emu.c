/* PRetroCalc OS - EMU launcher: pick a ROM, dispatch by extension. */
#include "apps.h"
#include "emu.h"
#include "os.h"
#include "gfx.h"
#include "keyboard.h"
#include "sound.h"
#include "sdfs.h"
#include "board.h"
#include "pico/stdlib.h"
#include <stdio.h>
#include <string.h>

static int ext_is(const char *name, const char *ext) {
    const char *dot = strrchr(name, '.');
    if (!dot || !dot[1]) return 0;
    const char *a = dot + 1, *b = ext;
    while (*a && *b) {
        int ca = *a++, cb = *b++;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return 0;
    }
    return *a == 0 && *b == 0;
}

#define EMU_DIR "EMU"
#define EMU_MAX 40

typedef struct {
    const char *ext;
    const char *sys;
    void (*run)(const char *path);
} emu_kind_t;

static const emu_kind_t kinds[] = {
    {"gb",  "GB",  emu_gb_run},
    {"gbc", "GBC", emu_gb_run},
};

static const emu_kind_t *kind_for(const char *name) {
    for (unsigned i = 0; i < sizeof kinds / sizeof kinds[0]; i++) {
        if (ext_is(name, kinds[i].ext)) return &kinds[i];
    }
    return NULL;
}

static void draw_frame(const char *title, int *cx, int *cy, int *cw, int *ch) {
    os_gem_desktop_bg();
    os_window(title, cx, cy, cw, ch);
}

static bool wait_key(int *code) {
    kbd_event_t ev;
    for (;;) {
        kbd_poll();
        sound_update();
        if (kbd_get_event(&ev) && ev.type == KBD_EV_PRESS) {
            *code = ev.code;
            return true;
        }
        sleep_ms(4);
    }
}

static void help_screen(void) {
    int cx, cy, cw, ch;
    draw_frame("EMU  help", &cx, &cy, &cw, &ch);
    int y = cy + 6;
    gfx_puts_fit(cx + 4, y, "Put ROMs in EMU/ on the SD card", GEM_BLACK, GEM_WHITE, cw - 8);
    y += 16;
    gfx_puts_fit(cx + 4, y, ".gb   Game Boy", GEM_BLACK, GEM_WHITE, cw - 8); y += 10;
    gfx_puts_fit(cx + 4, y, ".gbc  Game Boy Color", GEM_BLACK, GEM_WHITE, cw - 8); y += 16;
    gfx_puts_fit(cx + 4, y, "In game:", GEM_DGRAY, GEM_WHITE, cw - 8); y += 12;
    gfx_puts_fit(cx + 4, y, "Arrows/WASD  D-pad", GEM_BLACK, GEM_WHITE, cw - 8); y += 10;
    gfx_puts_fit(cx + 4, y, "Z/Space/Joy  A", GEM_BLACK, GEM_WHITE, cw - 8); y += 10;
    gfx_puts_fit(cx + 4, y, "X            B", GEM_BLACK, GEM_WHITE, cw - 8); y += 10;
    gfx_puts_fit(cx + 4, y, "Enter        Start", GEM_BLACK, GEM_WHITE, cw - 8); y += 10;
    gfx_puts_fit(cx + 4, y, "Tab          Select", GEM_BLACK, GEM_WHITE, cw - 8); y += 10;
    gfx_puts_fit(cx + 4, y, "F1  help   ESC  quit", GEM_BLACK, GEM_WHITE, cw - 8);
    gfx_puts_fit(cx + 4, cy + ch - 10, "any key", GEM_DGRAY, GEM_WHITE, cw - 8);
    gfx_flush();
    int c; wait_key(&c);
}

void app_emu(void) {
    if (!os_sd_present) {
        int cx, cy, cw, ch;
        draw_frame("EMU", &cx, &cy, &cw, &ch);
        gfx_puts_fit(cx + 4, cy + 40, "No SD card inserted", GEM_BLACK, GEM_WHITE, cw - 8);
        gfx_puts_fit(cx + 4, cy + ch - 10, "ESC=close", GEM_DGRAY, GEM_WHITE, cw);
        gfx_flush();
        int c; wait_key(&c);
        return;
    }
    sdfs_mkdir(EMU_DIR);
    sdfs_mkdir("EMU/SAVES");

    char names[EMU_MAX][48];
    const emu_kind_t *sys[EMU_MAX];
    int sel = 0;

    for (;;) {
        char all[EMU_MAX][48];
        int nall = sdfs_list_dir(EMU_DIR, all, EMU_MAX, false);
        int n = 0;
        for (int i = 0; i < nall && n < EMU_MAX; i++) {
            int L = (int)strlen(all[i]);
            if (L < 1 || all[i][L - 1] == '/') continue;
            const emu_kind_t *k = kind_for(all[i]);
            if (!k) continue;
            memcpy(names[n], all[i], 48);
            sys[n] = k;
            n++;
        }

        int cx, cy, cw, ch;
        draw_frame("EMU", &cx, &cy, &cw, &ch);
        int vis = (ch - 28) / FONT_H;
        if (vis < 1) vis = 1;
        if (sel >= n && n > 0) sel = n - 1;
        if (sel < 0) sel = 0;
        int top = sel - vis + 1;
        if (top < 0) top = 0;
        int y = cy + 4;
        for (int i = 0; i < vis && top + i < n; i++) {
            int idx = top + i;
            uint8_t fg = (idx == sel) ? GEM_WHITE : GEM_BLACK;
            uint8_t bg = (idx == sel) ? GEM_GREEN : GEM_WHITE;
            gfx_fill_rect(cx, y, cw, FONT_H, bg);
            char line[56];
            snprintf(line, sizeof line, "%-4s %s", sys[idx]->sys, names[idx]);
            gfx_puts_fit(cx + 4, y, line, fg, bg, cw - 8);
            y += FONT_H;
        }
        if (n == 0) {
            gfx_puts_fit(cx + 4, cy + 24, "No ROMs in EMU/", GEM_BLACK, GEM_WHITE, cw - 8);
            gfx_puts_fit(cx + 4, cy + 40, "Copy .gb / .gbc files to the", GEM_DGRAY, GEM_WHITE, cw - 8);
            gfx_puts_fit(cx + 4, cy + 50, "EMU folder on the SD card.", GEM_DGRAY, GEM_WHITE, cw - 8);
        }
        gfx_puts_fit(cx, cy + ch - 18, "ENTER=play  F1=help  ESC=close", GEM_DGRAY, GEM_WHITE, cw);
        gfx_flush();

        int c; wait_key(&c);
        sound_click();
        if (c == KEY_ESC) return;
        if (c == KEY_F1) { help_screen(); continue; }
        if (c == KEY_UP && sel > 0) sel--;
        if (c == KEY_DOWN && sel < n - 1) sel++;
        if (c == KEY_ENTER && n > 0) {
            char path[64];
            snprintf(path, sizeof path, EMU_DIR "/%s", names[sel]);
            sys[sel]->run(path);
        }
    }
}
