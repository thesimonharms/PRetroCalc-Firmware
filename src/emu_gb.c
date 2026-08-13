#pragma GCC optimize("O3")

/* PRetroCalc OS - Game Boy / Game Boy Color emulator core + frontend.
 * Scanline PPU, MBC1/2/3/5. ROMs live in EMU/ on the SD card. */
#include "emu.h"
#include "gfx.h"
#include "keyboard.h"
#include "sound.h"
#include "os.h"
#include "sdfs.h"
#include "board.h"
#include "pico/stdlib.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(ESP_PLATFORM)
#include <esp_heap_caps.h>
#include <esp_partition.h>
#include <esp_spi_flash.h>
#include <esp_timer.h>
#include <esp_task_wdt.h>
#include "s3_pie.h"
#ifndef ESP_PARTITION_SUBTYPE_ANY
#define ESP_PARTITION_SUBTYPE_ANY ((esp_partition_subtype_t)0xff)
#endif
#endif

#define Z_FLAG 0x80
#define N_FLAG 0x40
#define H_FLAG 0x20
#define C_FLAG 0x10

#define GB_W 160
#define GB_H 144
#define SCALE 2
#define ORIGIN_Y 16

enum { MBC_NONE = 0, MBC_1 = 1, MBC_2 = 2, MBC_3 = 3, MBC_5 = 5 };

typedef struct {
    uint16_t af, bc, de, hl, sp, pc;
    uint8_t ime, ime_sched, halt;
    uint8_t double_speed, speed_armed;
    uint8_t cgb;
    uint8_t mbc, ram_en, mbc_mode;
    uint16_t rom_bank, ram_bank;
    uint32_t rom_banks, rom_size, cram_size;
    uint8_t *rom;
    uint8_t *cram;
    uint8_t wram[0x8000];
    uint8_t vram[0x4000];
    uint8_t oam[0xA0];
    uint8_t hram[0x7F];
    uint8_t io[0x80];
    uint8_t ie;
    uint8_t vbk, svbk;
    uint8_t bg_pal[64], ob_pal[64];
    uint8_t bcps, ocps;
    uint16_t hdma_src, hdma_dst;
    int hdma_len; /* bytes remaining, -1 inactive */
    uint32_t div_cc, tima_cc, lcd_off_cy, sys_clk;
    int ppu_dots, ppu_len, ppu_mode, win_line;
    uint8_t frame, lyc_stat;
    uint8_t joy; /* 1 = pressed: A B Sel Start R L U D in bits 0-7 */
    uint8_t cram_dirty;
    uint8_t present;
    uint8_t rom_owned; /* malloc'd ROM (not flash mmap) */
} gb_t;

static uint16_t pix565[GB_W] __attribute__((aligned(16)));
static uint16_t scaled565[GB_W * SCALE] __attribute__((aligned(16)));
static uint16_t pal_bg565[32], pal_ob565[32], dmg_565[4];
static const uint8_t *rom_lo, *rom_hi;
static uint8_t *rom_cache;
static uint8_t *vram_ptr, *wram_svbk_ptr;
static uint32_t rom_slot_off[8];
static uint16_t rom_slot_lru[8], rom_lru_tick;
static int rom_cache_slots, rom_cache_heap, rom_sram_copies;
#if defined(ESP_PLATFORM)
static spi_flash_mmap_handle_t rom_mmap;
static int rom_mmap_on;
#endif

static const uint8_t dmg_rgb[4][3] = {
    {155, 188, 15}, {139, 172, 15}, {48, 98, 48}, {15, 56, 15},
};

static inline uint8_t rA(const gb_t *g) { return (uint8_t)(g->af >> 8); }
static inline uint8_t rF(const gb_t *g) { return (uint8_t)(g->af & 0xF0); }
static inline void wA(gb_t *g, uint8_t v) { g->af = (uint16_t)((v << 8) | (g->af & 0x00FF)); }
static inline void wF(gb_t *g, uint8_t v) { g->af = (uint16_t)((g->af & 0xFF00) | (v & 0xF0)); }
static inline uint8_t rB(const gb_t *g) { return (uint8_t)(g->bc >> 8); }
static inline uint8_t rC(const gb_t *g) { return (uint8_t)g->bc; }
static inline void wB(gb_t *g, uint8_t v) { g->bc = (uint16_t)((v << 8) | (g->bc & 0x00FF)); }
static inline void wC(gb_t *g, uint8_t v) { g->bc = (uint16_t)((g->bc & 0xFF00) | v); }
static inline uint8_t rD(const gb_t *g) { return (uint8_t)(g->de >> 8); }
static inline uint8_t rE(const gb_t *g) { return (uint8_t)g->de; }
static inline void wD(gb_t *g, uint8_t v) { g->de = (uint16_t)((v << 8) | (g->de & 0x00FF)); }
static inline void wE(gb_t *g, uint8_t v) { g->de = (uint16_t)((g->de & 0xFF00) | v); }
static inline uint8_t rH(const gb_t *g) { return (uint8_t)(g->hl >> 8); }
static inline uint8_t rL(const gb_t *g) { return (uint8_t)g->hl; }
static inline void wH(gb_t *g, uint8_t v) { g->hl = (uint16_t)((v << 8) | (g->hl & 0x00FF)); }
static inline void wL(gb_t *g, uint8_t v) { g->hl = (uint16_t)((g->hl & 0xFF00) | v); }

static inline uint32_t rom_off(const gb_t *g, uint16_t addr) {
    uint32_t bank, off;
    if (addr < 0x4000) {
        bank = 0;
        if (g->mbc == MBC_1 && g->mbc_mode)
            bank = (uint32_t)(g->ram_bank & 3) << 5;
        off = bank * 0x4000u + addr;
    } else {
        bank = g->rom_bank;
        if (g->mbc == MBC_1)
            bank = (g->rom_bank & 0x1F) | ((uint32_t)(g->ram_bank & 3) << 5);
        if (bank == 0 && g->mbc != MBC_5) bank = 1;
        off = bank * 0x4000u + (addr & 0x3FFF);
    }
    if (off >= g->rom_size) off %= g->rom_size ? g->rom_size : 1;
    return off;
}

static void rom_cache_free(void) {
    if (rom_cache_heap && rom_cache) {
#if defined(ESP_PLATFORM)
        heap_caps_free(rom_cache);
#else
        free(rom_cache);
#endif
    }
    rom_cache = NULL;
    rom_cache_heap = 0;
    rom_cache_slots = 0;
    rom_lo = rom_hi = NULL;
}

/* Copy flash-mapped ROM banks into SRAM. CGB cannot steal WRAM for this, so we
 * malloc 1–3×16KB: slot 0 is the fixed 0000–3FFF window, the rest LRU for
 * 4000–7FFF. Cap copies per frame so a name-screen bankstorm cannot stall. */
static void rom_cache_alloc(gb_t *g) {
    rom_cache_free();
    for (int i = 0; i < 8; i++) rom_slot_off[i] = ~0u;
    if (!g->rom || g->rom_owned) return;
#if defined(ESP_PLATFORM)
    static const int want[] = {3, 2, 1};
    for (int k = 0; k < 3; k++) {
        size_t n = (size_t)want[k] << 14;
        uint8_t *p = (uint8_t *)heap_caps_aligned_alloc(16, n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!p) p = (uint8_t *)heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (p) {
            rom_cache = p;
            rom_cache_slots = want[k];
            rom_cache_heap = 1;
            return;
        }
    }
#endif
}

static const uint8_t *rom_cache_map(gb_t *g, uint32_t off, int for_lo) {
    if (!g->rom) return NULL;
    if (g->rom_owned || !rom_cache || rom_cache_slots <= 0)
        return g->rom + off;
    for (int i = 0; i < rom_cache_slots; i++) {
        if (rom_slot_off[i] == off) {
            rom_slot_lru[i] = ++rom_lru_tick;
            return rom_cache + (i << 14);
        }
    }
    if (rom_sram_copies <= 0) return g->rom + off;
    int slot;
    if (for_lo) {
        /* One slot: keep it for 4000–7FFF (bank 0 sits in flash DCache better). */
        if (rom_cache_slots < 2) return g->rom + off;
        slot = 0;
    } else if (rom_cache_slots < 2) {
        slot = 0;
    } else {
        slot = 1;
        uint16_t best = 0xFFFF;
        for (int i = 1; i < rom_cache_slots; i++) {
            if (rom_slot_off[i] == ~0u) { slot = i; break; }
            if (rom_slot_lru[i] <= best) { best = rom_slot_lru[i]; slot = i; }
        }
    }
    uint8_t *dst = rom_cache + (slot << 14);
#if defined(ESP_PLATFORM)
    s3_pie_memcpy(dst, g->rom + off, 0x4000);
#else
    memcpy(dst, g->rom + off, 0x4000);
#endif
    rom_slot_off[slot] = off;
    rom_slot_lru[slot] = ++rom_lru_tick;
    rom_sram_copies--;
    return dst;
}

static void rom_cache_sync(gb_t *g) {
    if (!g->rom || !g->rom_size) {
        rom_lo = rom_hi = NULL;
        return;
    }
#if defined(ESP_PLATFORM)
    if (!g->rom_owned && !rom_cache && !g->cgb) {
        rom_cache = g->wram + 0x2000;
        rom_cache_slots = 1;
        rom_cache_heap = 0;
    }
#endif
    uint32_t lo = rom_off(g, 0);
    uint32_t hi = rom_off(g, 0x4000);
    rom_lo = rom_cache_map(g, lo, 1);
    rom_hi = rom_cache_map(g, hi, 0);
    if (!rom_lo) rom_lo = g->rom + lo;
    if (!rom_hi) rom_hi = g->rom + hi;
}

static void mem_map(gb_t *g) {
    vram_ptr = g->vram + ((uint32_t)g->vbk << 13);
    wram_svbk_ptr = g->wram + (uint32_t)(g->svbk ? g->svbk : 1) * 0x1000u;
}

static void gb_write(gb_t *g, uint16_t addr, uint8_t v);
static void pal_pack(const uint8_t *ram, uint16_t *out, int byte_i);

static uint8_t io_p1(const gb_t *g) {
    uint8_t p = g->io[0x00];
    uint8_t out = (uint8_t)(0xCF | (p & 0x30));
    uint8_t j = g->joy;
    if (!(p & 0x10)) { /* d-pad: R L U D in joy bits 4-7 */
        if (j & 0x10) out &= (uint8_t)~0x01;
        if (j & 0x20) out &= (uint8_t)~0x02;
        if (j & 0x40) out &= (uint8_t)~0x04;
        if (j & 0x80) out &= (uint8_t)~0x08;
    }
    if (!(p & 0x20)) { /* A B Select Start */
        if (j & 0x01) out &= (uint8_t)~0x01;
        if (j & 0x02) out &= (uint8_t)~0x02;
        if (j & 0x04) out &= (uint8_t)~0x04;
        if (j & 0x08) out &= (uint8_t)~0x08;
    }
    return out;
}

/* ---- Game Boy APU (4 channels → PWM DAC) ---- */
#define APU_CLK 4194304
static const uint8_t sq_duty[4][8] = {
    {0,0,0,0,0,0,0,1}, {1,0,0,0,0,0,0,1},
    {1,0,0,0,0,1,1,1}, {0,1,1,1,1,1,1,0},
};
static const uint8_t noise_div[8] = {8,16,32,48,64,80,96,112};
static const uint8_t nr_rmask[23] = {
    0x80,0x3F,0x00,0xFF,0xBF, 0xFF,0x3F,0x00,0xFF,0xBF,
    0x7F,0xFF,0x9F,0xFF,0xBF, 0xFF,0xFF,0x00,0x00,0xBF,
    0x00,0x00,0x70,
};

typedef struct {
    int on, dac, vol, duty, duty_i;
    int len, len_en, period, timer;
    int env_per, env_dir, env_cnt, env_vol;
    int sw_per, sw_dir, sw_shift, sw_cnt, sw_en, sw_shadow;
    int wave_i, wave_shift;
    uint16_t lfsr;
    int noise_wide;
    uint16_t freq;
} apu_ch_t;

static struct {
    apu_ch_t ch[4];
    int master, seq, seq_cy, pending;
    int32_t samp_acc;
    uint8_t nr50, nr51;
} apu;

static int apu_period_sq(uint16_t freq) {
    int n = 2048 - (int)(freq & 0x7FF);
    if (n < 1) n = 1;
    return n * 4;
}
static int apu_period_wave(uint16_t freq) {
    int n = 2048 - (int)(freq & 0x7FF);
    if (n < 1) n = 1;
    return n * 2;
}
static int apu_period_noise(uint8_t nr43) {
    int sh = nr43 >> 4, r = nr43 & 7;
    if (sh > 13) return 0;
    int p = (int)noise_div[r] << sh;
    return p < 8 ? 8 : p;
}

static void apu_reload_env(apu_ch_t *c, uint8_t nr) {
    c->env_vol = nr >> 4;
    c->vol = c->env_vol;
    c->env_dir = (nr >> 3) & 1;
    c->env_per = nr & 7;
    c->env_cnt = c->env_per;
    c->dac = (nr & 0xF8) != 0;
    if (!c->dac) c->on = 0;
}

static int apu_sweep_calc(apu_ch_t *c) {
    int f = c->sw_shadow;
    int d = f >> c->sw_shift;
    f = c->sw_dir ? (f - d) : (f + d);
    if (f < 0 || f > 2047) { c->on = 0; return -1; }
    return f;
}

static void apu_trigger(gb_t *g, int i) {
    apu_ch_t *c = &apu.ch[i];
    uint8_t *io = g->io;
    c->on = 1;
    if (i < 2) {
        uint8_t nrx2 = io[i == 0 ? 0x12 : 0x17];
        apu_reload_env(c, nrx2);
        c->duty = (io[i == 0 ? 0x11 : 0x16] >> 6) & 3;
        c->freq = (uint16_t)(io[i == 0 ? 0x13 : 0x18] | ((io[i == 0 ? 0x14 : 0x19] & 7) << 8));
        c->period = apu_period_sq(c->freq);
        c->timer = c->period;
        c->duty_i = 0;
        if (c->len == 0) c->len = 64;
        if (i == 0) {
            uint8_t nr10 = io[0x10];
            c->sw_per = (nr10 >> 4) & 7;
            c->sw_dir = (nr10 >> 3) & 1;
            c->sw_shift = nr10 & 7;
            c->sw_shadow = c->freq;
            c->sw_cnt = c->sw_per ? c->sw_per : 8;
            c->sw_en = c->sw_per || c->sw_shift;
            if (c->sw_shift) apu_sweep_calc(c);
        }
    } else if (i == 2) {
        c->dac = (io[0x1A] & 0x80) != 0;
        if (!c->dac) { c->on = 0; return; }
        c->freq = (uint16_t)(io[0x1D] | ((io[0x1E] & 7) << 8));
        c->period = apu_period_wave(c->freq);
        c->timer = c->period;
        c->wave_i = 0;
        {
            int v = (io[0x1C] >> 5) & 3;
            c->wave_shift = (v == 0) ? 4 : (v - 1);
        }
        if (c->len == 0) c->len = 256;
    } else {
        apu_reload_env(c, io[0x21]);
        c->lfsr = 0x7FFF;
        c->noise_wide = (io[0x22] >> 3) & 1;
        c->period = apu_period_noise(io[0x22]);
        if (!c->period) { c->on = 0; return; }
        c->timer = c->period;
        if (c->len == 0) c->len = 64;
    }
    if (!c->dac) c->on = 0;
}

static void apu_clock_len(void) {
    for (int i = 0; i < 4; i++) {
        apu_ch_t *c = &apu.ch[i];
        if (c->len_en && c->len > 0) {
            if (--c->len == 0) c->on = 0;
        }
    }
}
static void apu_clock_sweep(gb_t *g) {
    apu_ch_t *c = &apu.ch[0];
    if (!c->on || !c->sw_en) return;
    if (c->sw_cnt > 0) c->sw_cnt--;
    if (c->sw_cnt != 0) return;
    c->sw_cnt = c->sw_per ? c->sw_per : 8;
    if (c->sw_per == 0) return;
    int f = apu_sweep_calc(c);
    if (f < 0) return;
    if (c->sw_shift) {
        c->sw_shadow = f;
        c->freq = (uint16_t)f;
        g->io[0x13] = (uint8_t)f;
        g->io[0x14] = (uint8_t)((g->io[0x14] & 0xF8) | ((f >> 8) & 7));
        c->period = apu_period_sq(c->freq);
        apu_sweep_calc(c);
    }
}
static void apu_clock_env(void) {
    for (int i = 0; i < 4; i++) {
        if (i == 2) continue;
        apu_ch_t *c = &apu.ch[i];
        if (!c->on || c->env_per == 0) continue;
        if (--c->env_cnt > 0) continue;
        c->env_cnt = c->env_per;
        int v = c->vol + (c->env_dir ? 1 : -1);
        if (v >= 0 && v <= 15) c->vol = v;
    }
}

static void apu_write(gb_t *g, uint8_t r, uint8_t v) {
    if (!apu.master && r != 0x26) {
        if (r >= 0x30 && r <= 0x3F) g->io[r] = v;
        return;
    }
    g->io[r] = v;
    switch (r) {
    case 0x10: break;
    case 0x11: apu.ch[0].duty = v >> 6; apu.ch[0].len = 64 - (v & 63); break;
    case 0x12: apu_reload_env(&apu.ch[0], v); break;
    case 0x13: apu.ch[0].freq = (uint16_t)(v | ((g->io[0x14] & 7) << 8)); apu.ch[0].period = apu_period_sq(apu.ch[0].freq); break;
    case 0x14:
        apu.ch[0].len_en = (v >> 6) & 1;
        apu.ch[0].freq = (uint16_t)(g->io[0x13] | ((v & 7) << 8));
        apu.ch[0].period = apu_period_sq(apu.ch[0].freq);
        if (v & 0x80) apu_trigger(g, 0);
        break;
    case 0x16: apu.ch[1].duty = v >> 6; apu.ch[1].len = 64 - (v & 63); break;
    case 0x17: apu_reload_env(&apu.ch[1], v); break;
    case 0x18: apu.ch[1].freq = (uint16_t)(v | ((g->io[0x19] & 7) << 8)); apu.ch[1].period = apu_period_sq(apu.ch[1].freq); break;
    case 0x19:
        apu.ch[1].len_en = (v >> 6) & 1;
        apu.ch[1].freq = (uint16_t)(g->io[0x18] | ((v & 7) << 8));
        apu.ch[1].period = apu_period_sq(apu.ch[1].freq);
        if (v & 0x80) apu_trigger(g, 1);
        break;
    case 0x1A:
        apu.ch[2].dac = (v & 0x80) != 0;
        if (!apu.ch[2].dac) apu.ch[2].on = 0;
        break;
    case 0x1B: apu.ch[2].len = 256 - v; break;
    case 0x1C: {
        int s = (v >> 5) & 3;
        apu.ch[2].wave_shift = (s == 0) ? 4 : (s - 1);
        break;
    }
    case 0x1D: apu.ch[2].freq = (uint16_t)(v | ((g->io[0x1E] & 7) << 8)); apu.ch[2].period = apu_period_wave(apu.ch[2].freq); break;
    case 0x1E:
        apu.ch[2].len_en = (v >> 6) & 1;
        apu.ch[2].freq = (uint16_t)(g->io[0x1D] | ((v & 7) << 8));
        apu.ch[2].period = apu_period_wave(apu.ch[2].freq);
        if (v & 0x80) apu_trigger(g, 2);
        break;
    case 0x20: apu.ch[3].len = 64 - (v & 63); break;
    case 0x21: apu_reload_env(&apu.ch[3], v); break;
    case 0x22:
        apu.ch[3].noise_wide = (v >> 3) & 1;
        apu.ch[3].period = apu_period_noise(v);
        break;
    case 0x23:
        apu.ch[3].len_en = (v >> 6) & 1;
        if (v & 0x80) apu_trigger(g, 3);
        break;
    case 0x24: apu.nr50 = v; break;
    case 0x25: apu.nr51 = v; break;
    case 0x26:
        if (v & 0x80) {
            if (!apu.master) {
                apu.master = 1;
                apu.seq = 0;
                apu.seq_cy = 0;
            }
        } else if (apu.master) {
            apu.master = 0;
            memset(apu.ch, 0, sizeof apu.ch);
            memset(g->io + 0x10, 0, 0x16);
            apu.nr50 = apu.nr51 = 0;
        }
        break;
    default:
        break;
    }
}

static uint8_t apu_read(gb_t *g, uint8_t r) {
    if (r >= 0x30 && r <= 0x3F) return g->io[r];
    if (r == 0x26) {
        uint8_t v = (uint8_t)(0x70 | (apu.master ? 0x80 : 0));
        for (int i = 0; i < 4; i++) if (apu.ch[i].on) v |= (uint8_t)(1 << i);
        return v;
    }
    if (!apu.master) return 0xFF;
    if (r < 0x10 || r > 0x26) return g->io[r];
    return (uint8_t)(g->io[r] | nr_rmask[r - 0x10]);
}

static void apu_tick_ch(gb_t *g, int cy) {
    for (int i = 0; i < 2; i++) {
        apu_ch_t *c = &apu.ch[i];
        if (!c->on || c->period <= 0) continue;
        c->timer -= cy;
        if (c->timer <= 0) {
            int n = (-c->timer) / c->period + 1;
            c->timer += n * c->period;
            c->duty_i = (c->duty_i + n) & 7;
        }
    }
    {
        apu_ch_t *c = &apu.ch[2];
        if (c->on && c->period > 0) {
            c->timer -= cy;
            if (c->timer <= 0) {
                int n = (-c->timer) / c->period + 1;
                c->timer += n * c->period;
                c->wave_i = (c->wave_i + n) & 31;
            }
        }
    }
    {
        apu_ch_t *c = &apu.ch[3];
        if (c->on && c->period > 0) {
            c->timer -= cy;
            if (c->timer <= 0) {
                int n = (-c->timer) / c->period + 1;
                c->timer += n * c->period;
                while (n--) {
                    int xor = (c->lfsr ^ (c->lfsr >> 1)) & 1;
                    c->lfsr = (uint16_t)((c->lfsr >> 1) | (xor << 14));
                    if (c->noise_wide)
                        c->lfsr = (uint16_t)((c->lfsr & ~(1u << 6)) | ((uint16_t)xor << 6));
                }
            }
        }
    }
    (void)g;
}

static int apu_dac(gb_t *g, int i) {
    apu_ch_t *c = &apu.ch[i];
    if (!c->on || !c->dac) return 0;
    if (i < 2) return sq_duty[c->duty & 3][c->duty_i] ? c->vol : 0;
    if (i == 2) {
        uint8_t b = g->io[0x30 + (c->wave_i >> 1)];
        int s = (c->wave_i & 1) ? (b & 0x0F) : (b >> 4);
        return s >> c->wave_shift;
    }
    return (c->lfsr & 1) ? 0 : c->vol;
}

static void apu_mix_push(gb_t *g) {
    if (!apu.master) { sound_pcm_write(0, 0); return; }
    int s[4];
    for (int i = 0; i < 4; i++) s[i] = apu_dac(g, i);
    int l = 0, r = 0;
    uint8_t pan = apu.nr51;
    for (int i = 0; i < 4; i++) {
        if (pan & (1 << (i + 4))) l += s[i];
        if (pan & (1 << i)) r += s[i];
    }
    l *= (apu.nr50 >> 4) & 7;
    r *= apu.nr50 & 7;
    l = (l * 160) / 420;
    r = (r * 160) / 420;
    if (l > 255) l = 255;
    if (r > 255) r = 255;
    sound_pcm_write((uint8_t)l, (uint8_t)r);
}

static void apu_reset(gb_t *g) {
    memset(&apu, 0, sizeof apu);
    apu.nr50 = 0x77;
    apu.nr51 = 0xF3;
    g->io[0x24] = 0x77;
    g->io[0x25] = 0xF3;
    g->io[0x26] = 0xF0;
}

static void apu_step(gb_t *g, int cy) {
    int t = g->double_speed ? cy / 2 : cy;
    apu.seq_cy += t;
    while (apu.seq_cy >= 8192) {
        apu.seq_cy -= 8192;
        switch (apu.seq) {
        case 0: case 4: apu_clock_len(); break;
        case 2: case 6: apu_clock_len(); apu_clock_sweep(g); break;
        case 7: apu_clock_env(); break;
        default: break;
        }
        apu.seq = (apu.seq + 1) & 7;
    }
    apu.pending += t;
    apu.samp_acc += (int32_t)t * SOUND_PCM_RATE;
    while (apu.samp_acc >= APU_CLK) {
        apu.samp_acc -= APU_CLK;
        apu_tick_ch(g, apu.pending);
        apu.pending = 0;
        apu_mix_push(g);
    }
}

static inline uint8_t gb_read(gb_t *g, uint16_t addr) {
    if (addr < 0x4000) return rom_lo[addr];
    if (addr < 0x8000) return rom_hi[addr & 0x3FFF];
    if (addr < 0xA000) return vram_ptr[addr - 0x8000];
    if (addr < 0xC000) {
        if (!g->ram_en || !g->cram_size) return 0xFF;
        if (g->mbc == MBC_2) return g->cram[addr & 0x1FF] | 0xF0;
        if (g->mbc == MBC_3 && g->ram_bank >= 8) return 0;
        uint32_t a = (uint32_t)g->ram_bank * 0x2000u + (addr - 0xA000);
        if (a >= g->cram_size) return 0xFF;
        return g->cram[a];
    }
    if (addr < 0xD000) return g->wram[addr - 0xC000];
    if (addr < 0xE000) return wram_svbk_ptr[addr - 0xD000];
    if (addr < 0xFE00) return gb_read(g, (uint16_t)(addr - 0x2000));
    if (addr < 0xFEA0) return g->oam[addr - 0xFE00];
    if (addr < 0xFF00) return 0xFF;
    if (addr == 0xFF00) return io_p1(g);
    if (addr == 0xFF04) return (uint8_t)(g->div_cc >> 8);
    if (addr >= 0xFF10 && addr <= 0xFF3F) return apu_read(g, (uint8_t)(addr - 0xFF00));
    if (addr == 0xFF4F) return (uint8_t)(0xFE | g->vbk);
    if (addr == 0xFF70) return g->cgb ? (uint8_t)(0xF8 | g->svbk) : 0xFF;
    if (addr == 0xFF69) return g->bg_pal[g->bcps & 0x3F];
    if (addr == 0xFF6B) return g->ob_pal[g->ocps & 0x3F];
    if (addr < 0xFF80) return g->io[addr - 0xFF00];
    if (addr < 0xFFFF) return g->hram[addr - 0xFF80];
    return g->ie;
}

static inline uint8_t fetch8(gb_t *g) {
    uint16_t a = g->pc++;
    if (a < 0x4000) return rom_lo[a];
    if (a < 0x8000) return rom_hi[a & 0x3FFF];
    return gb_read(g, a);
}

static void mbc_write(gb_t *g, uint16_t addr, uint8_t v) {
    uint16_t rb = g->rom_bank;
    uint8_t ram = g->ram_bank, mode = g->mbc_mode;
    switch (g->mbc) {
    case MBC_NONE:
        break;
    case MBC_1:
        if (addr < 0x2000) g->ram_en = (v & 0x0F) == 0x0A;
        else if (addr < 0x4000) {
            g->rom_bank = (uint16_t)((g->rom_bank & 0xE0) | (v & 0x1F));
            if ((g->rom_bank & 0x1F) == 0) g->rom_bank |= 1;
        } else if (addr < 0x6000) g->ram_bank = v & 3;
        else g->mbc_mode = v & 1;
        break;
    case MBC_2:
        if (addr < 0x4000) {
            if (addr & 0x100) {
                g->rom_bank = (uint16_t)(v & 0x0F);
                if (g->rom_bank == 0) g->rom_bank = 1;
            } else g->ram_en = (v & 0x0F) == 0x0A;
        }
        break;
    case MBC_3:
        if (addr < 0x2000) g->ram_en = (v & 0x0F) == 0x0A;
        else if (addr < 0x4000) {
            g->rom_bank = (uint16_t)(v & 0x7F);
            if (g->rom_bank == 0) g->rom_bank = 1;
        } else if (addr < 0x6000) g->ram_bank = v & 0x0F;
        break;
    case MBC_5:
        if (addr < 0x2000) g->ram_en = (v & 0x0F) == 0x0A;
        else if (addr < 0x3000) g->rom_bank = (uint16_t)((g->rom_bank & 0x100) | v);
        else if (addr < 0x4000) g->rom_bank = (uint16_t)((g->rom_bank & 0xFF) | ((v & 1) << 8));
        else if (addr < 0x6000) g->ram_bank = v & 0x0F;
        break;
    default:
        break;
    }
    if (g->rom_bank != rb || g->mbc_mode != mode ||
        (g->mbc == MBC_1 && g->ram_bank != ram))
        rom_cache_sync(g);
}

static void oam_dma(gb_t *g, uint8_t src_hi) {
    uint16_t src = (uint16_t)(src_hi << 8);
    for (int i = 0; i < 0xA0; i++) g->oam[i] = gb_read(g, (uint16_t)(src + i));
}

static void hdma_copy16(gb_t *g) {
    if (g->hdma_len <= 0) return;
    int n = g->hdma_len < 16 ? g->hdma_len : 16;
    for (int i = 0; i < n; i++) {
        uint8_t b = gb_read(g, g->hdma_src);
        gb_write(g, (uint16_t)(0x8000 | (g->hdma_dst & 0x1FFF)), b);
        g->hdma_src++;
        g->hdma_dst++;
    }
    g->hdma_len -= n;
    if (g->hdma_len <= 0) {
        g->hdma_len = -1;
        g->io[0x55] = 0xFF;
    } else g->io[0x55] = (uint8_t)((g->hdma_len / 16) - 1);
}

static void gb_write(gb_t *g, uint16_t addr, uint8_t v) {
    if (addr < 0x8000) { mbc_write(g, addr, v); return; }
    if (addr < 0xA000) {
        vram_ptr[addr - 0x8000] = v;
        return;
    }
    if (addr < 0xC000) {
        if (!g->ram_en || !g->cram_size) return;
        if (g->mbc == MBC_2) {
            g->cram[addr & 0x1FF] = v & 0x0F;
            g->cram_dirty = 1;
            return;
        }
        if (g->mbc == MBC_3 && g->ram_bank >= 8) return;
        uint32_t a = (uint32_t)g->ram_bank * 0x2000u + (addr - 0xA000);
        if (a < g->cram_size) { g->cram[a] = v; g->cram_dirty = 1; }
        return;
    }
    if (addr < 0xD000) { g->wram[addr - 0xC000] = v; return; }
    if (addr < 0xE000) {
        wram_svbk_ptr[addr - 0xD000] = v;
        return;
    }
    if (addr < 0xFE00) { gb_write(g, (uint16_t)(addr - 0x2000), v); return; }
    if (addr < 0xFEA0) { g->oam[addr - 0xFE00] = v; return; }
    if (addr < 0xFF00) return;
    if (addr == 0xFFFF) { g->ie = v; return; }
    if (addr >= 0xFF80) { g->hram[addr - 0xFF80] = v; return; }

    uint8_t r = (uint8_t)(addr - 0xFF00);
    if (r >= 0x10 && r <= 0x3F) { apu_write(g, r, v); return; }
    switch (r) {
    case 0x00: g->io[0] = (uint8_t)(v | 0xC0); break;
    case 0x04: g->div_cc = 0; g->io[4] = 0; break;
    case 0x05: g->io[5] = v; break;
    case 0x06: g->io[6] = v; break;
    case 0x07: g->io[7] = v; break;
    case 0x0F: g->io[0x0F] = (uint8_t)(v | 0xE0); break;
    case 0x40: {
        uint8_t was = g->io[0x40] & 0x80;
        g->io[0x40] = v;
        if (was && !(v & 0x80)) {
            g->io[0x44] = 0;
            g->ppu_mode = 0;
            g->ppu_dots = 0;
            g->ppu_len = 456;
            g->io[0x41] = (uint8_t)((g->io[0x41] & 0xFC) | 0);
        }
        if (!was && (v & 0x80)) {
            g->io[0x44] = 0;
            g->ppu_mode = 2;
            g->ppu_dots = 0;
            g->ppu_len = 80;
            g->win_line = 0;
        }
        break;
    }
    case 0x41: g->io[0x41] = (uint8_t)((v & 0xF8) | (g->io[0x41] & 0x07)); break;
    case 0x44: break;
    case 0x45: g->io[0x45] = v; break;
    case 0x46: oam_dma(g, v); g->io[0x46] = v; break;
    case 0x4D:
        if (g->cgb) g->speed_armed = v & 1;
        g->io[0x4D] = (uint8_t)((g->double_speed ? 0x80 : 0) | (v & 1));
        break;
    case 0x4F:
        if (g->cgb) { g->vbk = v & 1; mem_map(g); }
        break;
    case 0x51: g->hdma_src = (uint16_t)((v << 8) | (g->hdma_src & 0xFF)); break;
    case 0x52: g->hdma_src = (uint16_t)((g->hdma_src & 0xFF00) | (v & 0xF0)); break;
    case 0x53: g->hdma_dst = (uint16_t)((v << 8) | (g->hdma_dst & 0xFF)); break;
    case 0x54: g->hdma_dst = (uint16_t)((g->hdma_dst & 0xFF00) | (v & 0xF0)); break;
    case 0x55:
        if (g->cgb) {
            int len = ((v & 0x7F) + 1) * 16;
            g->hdma_src &= 0xFFF0;
            g->hdma_dst = (uint16_t)(0x8000 | (g->hdma_dst & 0x1FF0));
            if (v & 0x80) {
                g->hdma_len = len;
                g->io[0x55] = v & 0x7F;
            } else {
                g->hdma_len = len;
                while (g->hdma_len > 0) hdma_copy16(g);
            }
        }
        break;
    case 0x68: g->bcps = v; g->io[0x68] = v; break;
    case 0x69:
        g->bg_pal[g->bcps & 0x3F] = v;
        pal_pack(g->bg_pal, pal_bg565, g->bcps & 0x3F);
        if (g->bcps & 0x80) g->bcps = (uint8_t)((g->bcps & 0x80) | ((g->bcps + 1) & 0x3F));
        break;
    case 0x6A: g->ocps = v; g->io[0x6A] = v; break;
    case 0x6B:
        g->ob_pal[g->ocps & 0x3F] = v;
        pal_pack(g->ob_pal, pal_ob565, g->ocps & 0x3F);
        if (g->ocps & 0x80) g->ocps = (uint8_t)((g->ocps & 0x80) | ((g->ocps + 1) & 0x3F));
        break;
    case 0x70:
        if (g->cgb) { g->svbk = v & 7; mem_map(g); }
        break;
    default:
        g->io[r] = v;
        break;
    }
}

static inline uint16_t rgb_to_565be(uint8_t r, uint8_t gr, uint8_t b) {
    uint16_t p = (uint16_t)(((r & 0xF8) << 8) | ((gr & 0xFC) << 3) | (b >> 3));
    return (uint16_t)((p << 8) | (p >> 8));
}

static void pal_pack(const uint8_t *ram, uint16_t *out, int byte_i) {
    int i = byte_i >> 1;
    uint16_t c = (uint16_t)(ram[i * 2] | (ram[i * 2 + 1] << 8));
    unsigned r5 = c & 31, g5 = (c >> 5) & 31, b5 = (c >> 10) & 31;
    uint16_t p = (uint16_t)((r5 << 11) | (((g5 << 1) | (g5 >> 4)) << 5) | b5);
    out[i] = (uint16_t)((p << 8) | (p >> 8));
}

static void pal_rebuild(gb_t *g) {
    for (int i = 0; i < 32; i++) {
        pal_pack(g->bg_pal, pal_bg565, i * 2);
        pal_pack(g->ob_pal, pal_ob565, i * 2);
    }
}

static void emit_line(gb_t *g, int ly) {
    if (!g->present) return;
    if (ly == 0) gfx_direct_begin_565(0, ORIGIN_Y, GB_W * SCALE, GB_H * SCALE);
#if defined(ESP_PLATFORM)
    s3_pie_scale2_u16(scaled565, pix565, GB_W);
#else
    for (int x = 0; x < GB_W; x++)
        scaled565[x * 2] = scaled565[x * 2 + 1] = pix565[x];
#endif
    gfx_direct_rgb565(scaled565, GB_W * SCALE);
    gfx_direct_rgb565(scaled565, GB_W * SCALE);
}

static uint8_t dmg_shade(uint8_t pal, uint8_t c) {
    return (uint8_t)((pal >> (c * 2)) & 3);
}

static void tile_row(const uint8_t *td, int y, int xflip, uint8_t pix[8]) {
    uint8_t lo = td[y * 2], hi = td[y * 2 + 1];
    if (!xflip) {
        pix[0] = (uint8_t)(((hi >> 7) & 1) << 1 | ((lo >> 7) & 1));
        pix[1] = (uint8_t)(((hi >> 6) & 1) << 1 | ((lo >> 6) & 1));
        pix[2] = (uint8_t)(((hi >> 5) & 1) << 1 | ((lo >> 5) & 1));
        pix[3] = (uint8_t)(((hi >> 4) & 1) << 1 | ((lo >> 4) & 1));
        pix[4] = (uint8_t)(((hi >> 3) & 1) << 1 | ((lo >> 3) & 1));
        pix[5] = (uint8_t)(((hi >> 2) & 1) << 1 | ((lo >> 2) & 1));
        pix[6] = (uint8_t)(((hi >> 1) & 1) << 1 | ((lo >> 1) & 1));
        pix[7] = (uint8_t)(((hi >> 0) & 1) << 1 | ((lo >> 0) & 1));
    } else {
        pix[0] = (uint8_t)(((hi >> 0) & 1) << 1 | ((lo >> 0) & 1));
        pix[1] = (uint8_t)(((hi >> 1) & 1) << 1 | ((lo >> 1) & 1));
        pix[2] = (uint8_t)(((hi >> 2) & 1) << 1 | ((lo >> 2) & 1));
        pix[3] = (uint8_t)(((hi >> 3) & 1) << 1 | ((lo >> 3) & 1));
        pix[4] = (uint8_t)(((hi >> 4) & 1) << 1 | ((lo >> 4) & 1));
        pix[5] = (uint8_t)(((hi >> 5) & 1) << 1 | ((lo >> 5) & 1));
        pix[6] = (uint8_t)(((hi >> 6) & 1) << 1 | ((lo >> 6) & 1));
        pix[7] = (uint8_t)(((hi >> 7) & 1) << 1 | ((lo >> 7) & 1));
    }
}

static const uint8_t *tile_data(gb_t *g, uint8_t tile, int bank, int signed_mode) {
    uint32_t base;
    if (signed_mode) base = (uint32_t)(0x1000 + (int8_t)tile * 16);
    else base = (uint32_t)tile * 16;
    return &g->vram[(uint32_t)bank * 0x2000u + base];
}

static void ppu_render(gb_t *g, int ly) {
    uint8_t lcdc = g->io[0x40];
    uint8_t bgc[GB_W], bgp[GB_W], bgpr[GB_W];
    memset(bgc, 0, sizeof bgc);
    memset(bgp, 0, sizeof bgp);
    memset(bgpr, 0, sizeof bgpr);
    int bg_on = g->cgb || (lcdc & 0x01);
    int win_on = (lcdc & 0x20) && (g->cgb || (lcdc & 0x01));
    uint8_t scx = g->io[0x43], scy = g->io[0x42];
    uint8_t wy = g->io[0x4A], wx = g->io[0x4B];
    int signed_tiles = !(lcdc & 0x10);
    int bg_map = (lcdc & 0x08) ? 0x1C00 : 0x1800;
    int win_map = (lcdc & 0x40) ? 0x1C00 : 0x1800;
    int win_x0 = (int)wx - 7;
    int do_win = win_on && ly >= wy && win_x0 < GB_W;
    int wline = g->win_line;

    if (bg_on) {
        int y = (ly + scy) & 255;
        int ty = y >> 3, row = y & 7;
        for (int x = 0; x < GB_W; ) {
            int px = (x + scx) & 255;
            int tx = px >> 3, col = px & 7;
            uint32_t me = (uint32_t)bg_map + (uint32_t)ty * 32 + (uint32_t)tx;
            uint8_t tile = g->vram[me];
            uint8_t attr = g->cgb ? g->vram[me + 0x2000] : 0;
            int bank = (attr >> 3) & 1;
            int yf = (attr & 0x40) ? 7 - row : row;
            uint8_t pix[8];
            tile_row(tile_data(g, tile, bank, signed_tiles), yf, attr & 0x20, pix);
            for (; col < 8 && x < GB_W; col++, x++) {
                uint8_t c = pix[col];
                bgc[x] = c;
                bgp[x] = attr & 7;
                bgpr[x] = (attr >> 7) & 1;
            }
        }
    }
    if (do_win) {
        int y = wline;
        int ty = y >> 3, row = y & 7;
        int x0 = win_x0 < 0 ? 0 : win_x0;
        for (int x = x0; x < GB_W; ) {
            int px = x - win_x0;
            int tx = px >> 3, col = px & 7;
            uint32_t me = (uint32_t)win_map + (uint32_t)ty * 32 + (uint32_t)tx;
            uint8_t tile = g->vram[me];
            uint8_t attr = g->cgb ? g->vram[me + 0x2000] : 0;
            int bank = (attr >> 3) & 1;
            int yf = (attr & 0x40) ? 7 - row : row;
            uint8_t pix[8];
            tile_row(tile_data(g, tile, bank, signed_tiles), yf, attr & 0x20, pix);
            for (; col < 8 && x < GB_W; col++, x++) {
                uint8_t c = pix[col];
                bgc[x] = c;
                bgp[x] = attr & 7;
                bgpr[x] = (attr >> 7) & 1;
            }
        }
        g->win_line++;
    }

    if (g->cgb) {
        for (int x = 0; x < GB_W; x++)
            pix565[x] = pal_bg565[bgp[x] * 4 + bgc[x]];
    } else {
        uint8_t pal = g->io[0x47];
        for (int x = 0; x < GB_W; x++)
            pix565[x] = dmg_565[dmg_shade(pal, bgc[x])];
    }

    if (lcdc & 0x02) {
        int h = (lcdc & 0x04) ? 16 : 8;
        int list[10], n = 0;
        for (int i = 0; i < 40 && n < 10; i++) {
            int sy = g->oam[i * 4] - 16;
            if (ly >= sy && ly < sy + h) list[n++] = i;
        }
        for (int s = n - 1; s >= 0; s--) {
            int i = list[s];
            int sy = g->oam[i * 4] - 16;
            int sx = g->oam[i * 4 + 1] - 8;
            uint8_t tile = g->oam[i * 4 + 2];
            uint8_t attr = g->oam[i * 4 + 3];
            int row = ly - sy;
            if (attr & 0x40) row = h - 1 - row;
            if (h == 16) {
                tile &= 0xFE;
                if (row >= 8) { tile |= 1; row -= 8; }
            }
            int bank = g->cgb ? ((attr >> 3) & 1) : 0;
            uint8_t pix[8];
            tile_row(tile_data(g, tile, bank, 0), row, attr & 0x20, pix);
            for (int col = 0; col < 8; col++) {
                int x = sx + col;
                if ((unsigned)x >= GB_W) continue;
                uint8_t c = pix[col];
                if (!c) continue;
                int bg_over = 0;
                if (g->cgb) {
                    if (!(lcdc & 0x01)) bg_over = 0;
                    else if (bgpr[x] && bgc[x]) bg_over = 1;
                    else if ((attr & 0x80) && bgc[x]) bg_over = 1;
                } else if ((attr & 0x80) && bgc[x]) bg_over = 1;
                if (bg_over) continue;
                if (g->cgb) pix565[x] = pal_ob565[(attr & 7) * 4 + c];
                else {
                    uint8_t pal = g->io[(attr & 0x10) ? 0x49 : 0x48];
                    pix565[x] = dmg_565[dmg_shade(pal, c)];
                }
            }
        }
    }
    emit_line(g, ly);
}

static void ppu_skip_line(gb_t *g, int ly) {
    uint8_t lcdc = g->io[0x40];
    int win_on = (lcdc & 0x20) && (g->cgb || (lcdc & 0x01));
    uint8_t wy = g->io[0x4A], wx = g->io[0x4B];
    if (win_on && ly >= wy && (int)wx - 7 < GB_W) g->win_line++;
}

static void stat_irq(gb_t *g, int mode_bit) {
    if (g->io[0x41] & mode_bit) g->io[0x0F] |= 2;
}

static void check_lyc(gb_t *g) {
    uint8_t eq = g->io[0x44] == g->io[0x45];
    if (eq) g->io[0x41] |= 4;
    else g->io[0x41] &= (uint8_t)~4;
    if (eq && (g->io[0x41] & 0x40) && !g->lyc_stat) g->io[0x0F] |= 2;
    g->lyc_stat = eq;
}

static void ppu_set_mode(gb_t *g, int mode, int len) {
    g->ppu_mode = mode;
    g->ppu_len = len;
    g->io[0x41] = (uint8_t)((g->io[0x41] & 0xFC) | (mode & 3));
    if (mode == 0) stat_irq(g, 0x08);
    else if (mode == 1) stat_irq(g, 0x10);
    else if (mode == 2) stat_irq(g, 0x20);
}

static void gb_ppu_step(gb_t *g, int cy) {
    if (!(g->io[0x40] & 0x80)) {
        g->lcd_off_cy += (uint32_t)cy;
        if (g->lcd_off_cy >= 70224) {
            g->lcd_off_cy -= 70224;
            g->frame = 1;
        }
        return;
    }
    int dots = g->double_speed ? cy / 2 : cy;
    g->ppu_dots += dots;
    while (g->ppu_dots >= g->ppu_len) {
        g->ppu_dots -= g->ppu_len;
        switch (g->ppu_mode) {
        case 2:
            ppu_set_mode(g, 3, 172);
            break;
        case 3:
            if (g->present) ppu_render(g, g->io[0x44]);
            else ppu_skip_line(g, g->io[0x44]);
            ppu_set_mode(g, 0, 204);
            if (g->hdma_len > 0) hdma_copy16(g);
            break;
        case 0:
            g->io[0x44]++;
            if (g->io[0x44] == 144) {
                ppu_set_mode(g, 1, 456);
                g->io[0x0F] |= 1;
                g->frame = 1;
                g->win_line = 0;
            } else ppu_set_mode(g, 2, 80);
            check_lyc(g);
            break;
        default:
            g->io[0x44]++;
            if (g->io[0x44] > 153) {
                g->io[0x44] = 0;
                ppu_set_mode(g, 2, 80);
            } else g->ppu_len = 456;
            check_lyc(g);
            break;
        }
    }
}

static inline void gb_timer_step(gb_t *g, int cy) {
    g->div_cc += (uint32_t)cy;
    g->io[4] = (uint8_t)(g->div_cc >> 8);
    if (!(g->io[7] & 4)) return;
    static const int per[4] = {1024, 16, 64, 256};
    int p = per[g->io[7] & 3];
    g->tima_cc += (uint32_t)cy;
    while (g->tima_cc >= (uint32_t)p) {
        g->tima_cc -= (uint32_t)p;
        if ((++g->io[5]) == 0) {
            g->io[5] = g->io[6];
            g->io[0x0F] |= 4;
        }
    }
}

static inline uint8_t r8(gb_t *g, int i) {
    switch (i) {
    case 0: return rB(g);
    case 1: return rC(g);
    case 2: return rD(g);
    case 3: return rE(g);
    case 4: return rH(g);
    case 5: return rL(g);
    case 6: return gb_read(g, g->hl);
    default: return rA(g);
    }
}
static inline void w8(gb_t *g, int i, uint8_t v) {
    switch (i) {
    case 0: wB(g, v); break;
    case 1: wC(g, v); break;
    case 2: wD(g, v); break;
    case 3: wE(g, v); break;
    case 4: wH(g, v); break;
    case 5: wL(g, v); break;
    case 6: gb_write(g, g->hl, v); break;
    default: wA(g, v); break;
    }
}

static uint8_t inc8(gb_t *g, uint8_t v) {
    v++;
    uint8_t f = (uint8_t)(rF(g) & C_FLAG);
    if (!v) f |= Z_FLAG;
    if ((v & 0x0F) == 0) f |= H_FLAG;
    wF(g, f);
    return v;
}
static uint8_t dec8(gb_t *g, uint8_t v) {
    v--;
    uint8_t f = (uint8_t)((rF(g) & C_FLAG) | N_FLAG);
    if (!v) f |= Z_FLAG;
    if ((v & 0x0F) == 0x0F) f |= H_FLAG;
    wF(g, f);
    return v;
}

static void add_hl(gb_t *g, uint16_t v) {
    unsigned r = g->hl + v;
    uint8_t f = (uint8_t)(rF(g) & Z_FLAG);
    if ((g->hl & 0x0FFF) + (v & 0x0FFF) > 0x0FFF) f |= H_FLAG;
    if (r > 0xFFFF) f |= C_FLAG;
    wF(g, f);
    g->hl = (uint16_t)r;
}

static void alu(gb_t *g, int op, uint8_t v) {
    uint8_t a = rA(g);
    uint8_t f = 0;
    unsigned r;
    switch (op) {
    case 0: /* ADD */
        r = a + v;
        if ((uint8_t)r == 0) f |= Z_FLAG;
        if ((a & 0xF) + (v & 0xF) > 0xF) f |= H_FLAG;
        if (r > 0xFF) f |= C_FLAG;
        wA(g, (uint8_t)r);
        break;
    case 1: /* ADC */
        r = a + v + ((rF(g) & C_FLAG) ? 1 : 0);
        if ((uint8_t)r == 0) f |= Z_FLAG;
        if ((a & 0xF) + (v & 0xF) + ((rF(g) & C_FLAG) ? 1 : 0) > 0xF) f |= H_FLAG;
        if (r > 0xFF) f |= C_FLAG;
        wA(g, (uint8_t)r);
        break;
    case 2: /* SUB */
        r = a - v;
        f = N_FLAG;
        if ((uint8_t)r == 0) f |= Z_FLAG;
        if ((a & 0xF) < (v & 0xF)) f |= H_FLAG;
        if (a < v) f |= C_FLAG;
        wA(g, (uint8_t)r);
        break;
    case 3: { /* SBC */
        unsigned cy = (rF(g) & C_FLAG) ? 1 : 0;
        r = a - v - cy;
        f = N_FLAG;
        if ((uint8_t)r == 0) f |= Z_FLAG;
        if ((a & 0xF) < (v & 0xF) + cy) f |= H_FLAG;
        if (a < v + cy) f |= C_FLAG;
        wA(g, (uint8_t)r);
        break;
    }
    case 4:
        r = a & v;
        f = H_FLAG;
        if ((uint8_t)r == 0) f |= Z_FLAG;
        wA(g, (uint8_t)r);
        break;
    case 5:
        r = a ^ v;
        if ((uint8_t)r == 0) f |= Z_FLAG;
        wA(g, (uint8_t)r);
        break;
    case 6:
        r = a | v;
        if ((uint8_t)r == 0) f |= Z_FLAG;
        wA(g, (uint8_t)r);
        break;
    default:
        r = a - v;
        f = N_FLAG;
        if ((uint8_t)r == 0) f |= Z_FLAG;
        if ((a & 0xF) < (v & 0xF)) f |= H_FLAG;
        if (a < v) f |= C_FLAG;
        break;
    }
    wF(g, f);
}

static void daa(gb_t *g) {
    uint8_t a = rA(g), f = rF(g);
    if (!(f & N_FLAG)) {
        if ((f & C_FLAG) || a > 0x99) { a = (uint8_t)(a + 0x60); f |= C_FLAG; }
        if ((f & H_FLAG) || (a & 0x0F) > 9) a = (uint8_t)(a + 0x06);
    } else {
        if (f & C_FLAG) a = (uint8_t)(a - 0x60);
        if (f & H_FLAG) a = (uint8_t)(a - 0x06);
    }
    f &= (uint8_t)(C_FLAG | N_FLAG);
    if (!a) f |= Z_FLAG;
    wA(g, a);
    wF(g, f);
}

static inline uint16_t fetch16(gb_t *g) {
    uint8_t lo = fetch8(g);
    uint8_t hi = fetch8(g);
    return (uint16_t)(lo | (hi << 8));
}

static void push16(gb_t *g, uint16_t v) {
    gb_write(g, --g->sp, (uint8_t)(v >> 8));
    gb_write(g, --g->sp, (uint8_t)v);
}
static uint16_t pop16(gb_t *g) {
    uint8_t lo = gb_read(g, g->sp++);
    uint8_t hi = gb_read(g, g->sp++);
    return (uint16_t)(lo | (hi << 8));
}

static int cond(gb_t *g, int cc) {
    uint8_t f = rF(g);
    switch (cc) {
    case 0: return !(f & Z_FLAG);
    case 1: return (f & Z_FLAG) != 0;
    case 2: return !(f & C_FLAG);
    default: return (f & C_FLAG) != 0;
    }
}

static uint8_t cb_rot(gb_t *g, int op, uint8_t v) {
    uint8_t c, f = 0, r = v;
    switch (op) {
    case 0: c = v >> 7; r = (uint8_t)((v << 1) | c); if (c) f |= C_FLAG; break;
    case 1: c = v & 1; r = (uint8_t)((v >> 1) | (c << 7)); if (c) f |= C_FLAG; break;
    case 2: c = v >> 7; r = (uint8_t)((v << 1) | ((rF(g) & C_FLAG) ? 1 : 0)); if (c) f |= C_FLAG; break;
    case 3: c = v & 1; r = (uint8_t)((v >> 1) | ((rF(g) & C_FLAG) ? 0x80 : 0)); if (c) f |= C_FLAG; break;
    case 4: c = v >> 7; r = (uint8_t)(v << 1); if (c) f |= C_FLAG; break;
    case 5: c = v & 1; r = (uint8_t)((v >> 1) | (v & 0x80)); if (c) f |= C_FLAG; break;
    case 6: r = (uint8_t)((v << 4) | (v >> 4)); break;
    default: c = v & 1; r = (uint8_t)(v >> 1); if (c) f |= C_FLAG; break;
    }
    if (!r) f |= Z_FLAG;
    wF(g, f);
    return r;
}

static int gb_exec_cb(gb_t *g) {
    uint8_t op = fetch8(g);
    int reg = op & 7;
    int group = op >> 6;
    int n = (op >> 3) & 7;
    uint8_t v = r8(g, reg);
    int cy = (reg == 6) ? 16 : 8;
    if (group == 0) {
        w8(g, reg, cb_rot(g, n, v));
        return cy;
    }
    if (group == 1) {
        uint8_t f = (uint8_t)((rF(g) & C_FLAG) | H_FLAG);
        if (!((v >> n) & 1)) f |= Z_FLAG;
        wF(g, f);
        return (reg == 6) ? 12 : 8;
    }
    if (group == 2) w8(g, reg, (uint8_t)(v & ~(1u << n)));
    else w8(g, reg, (uint8_t)(v | (1u << n)));
    return cy;
}

static int gb_exec(gb_t *g) {
    uint8_t op = fetch8(g);
    int8_t e;
    uint16_t nn;
    if (op == 0xCB) return gb_exec_cb(g);
    if (op >= 0x40 && op < 0x80) {
        if (op == 0x76) { g->halt = 1; return 4; }
        w8(g, (op >> 3) & 7, r8(g, op & 7));
        return ((op & 7) == 6 || ((op >> 3) & 7) == 6) ? 8 : 4;
    }
    if (op >= 0x80 && op < 0xC0) {
        alu(g, (op >> 3) & 7, r8(g, op & 7));
        return ((op & 7) == 6) ? 8 : 4;
    }
    switch (op) {
    case 0x00: return 4;
    case 0x01: g->bc = fetch16(g); return 12;
    case 0x02: gb_write(g, g->bc, rA(g)); return 8;
    case 0x03: g->bc++; return 8;
    case 0x04: wB(g, inc8(g, rB(g))); return 4;
    case 0x05: wB(g, dec8(g, rB(g))); return 4;
    case 0x06: wB(g, fetch8(g)); return 8;
    case 0x07: {
        uint8_t a = rA(g), c = a >> 7;
        wA(g, (uint8_t)((a << 1) | c));
        wF(g, c ? C_FLAG : 0);
        return 4;
    }
    case 0x08: nn = fetch16(g); gb_write(g, nn, (uint8_t)g->sp); gb_write(g, (uint16_t)(nn + 1), (uint8_t)(g->sp >> 8)); return 20;
    case 0x09: add_hl(g, g->bc); return 8;
    case 0x0A: wA(g, gb_read(g, g->bc)); return 8;
    case 0x0B: g->bc--; return 8;
    case 0x0C: wC(g, inc8(g, rC(g))); return 4;
    case 0x0D: wC(g, dec8(g, rC(g))); return 4;
    case 0x0E: wC(g, fetch8(g)); return 8;
    case 0x0F: {
        uint8_t a = rA(g), c = a & 1;
        wA(g, (uint8_t)((a >> 1) | (c << 7)));
        wF(g, c ? C_FLAG : 0);
        return 4;
    }
    case 0x10:
        g->pc++;
        if (g->cgb && g->speed_armed) {
            g->double_speed ^= 1;
            g->speed_armed = 0;
            g->io[0x4D] = g->double_speed ? 0x80 : 0;
        }
        return 4;
    case 0x11: g->de = fetch16(g); return 12;
    case 0x12: gb_write(g, g->de, rA(g)); return 8;
    case 0x13: g->de++; return 8;
    case 0x14: wD(g, inc8(g, rD(g))); return 4;
    case 0x15: wD(g, dec8(g, rD(g))); return 4;
    case 0x16: wD(g, fetch8(g)); return 8;
    case 0x17: {
        uint8_t a = rA(g), c = a >> 7;
        wA(g, (uint8_t)((a << 1) | ((rF(g) & C_FLAG) ? 1 : 0)));
        wF(g, c ? C_FLAG : 0);
        return 4;
    }
    case 0x18: e = (int8_t)fetch8(g); g->pc = (uint16_t)(g->pc + e); return 12;
    case 0x19: add_hl(g, g->de); return 8;
    case 0x1A: wA(g, gb_read(g, g->de)); return 8;
    case 0x1B: g->de--; return 8;
    case 0x1C: wE(g, inc8(g, rE(g))); return 4;
    case 0x1D: wE(g, dec8(g, rE(g))); return 4;
    case 0x1E: wE(g, fetch8(g)); return 8;
    case 0x1F: {
        uint8_t a = rA(g), c = a & 1;
        wA(g, (uint8_t)((a >> 1) | ((rF(g) & C_FLAG) ? 0x80 : 0)));
        wF(g, c ? C_FLAG : 0);
        return 4;
    }
    case 0x20: e = (int8_t)fetch8(g); if (cond(g, 0)) { g->pc = (uint16_t)(g->pc + e); return 12; } return 8;
    case 0x21: g->hl = fetch16(g); return 12;
    case 0x22: gb_write(g, g->hl++, rA(g)); return 8;
    case 0x23: g->hl++; return 8;
    case 0x24: wH(g, inc8(g, rH(g))); return 4;
    case 0x25: wH(g, dec8(g, rH(g))); return 4;
    case 0x26: wH(g, fetch8(g)); return 8;
    case 0x27: daa(g); return 4;
    case 0x28: e = (int8_t)fetch8(g); if (cond(g, 1)) { g->pc = (uint16_t)(g->pc + e); return 12; } return 8;
    case 0x29: add_hl(g, g->hl); return 8;
    case 0x2A: wA(g, gb_read(g, g->hl++)); return 8;
    case 0x2B: g->hl--; return 8;
    case 0x2C: wL(g, inc8(g, rL(g))); return 4;
    case 0x2D: wL(g, dec8(g, rL(g))); return 4;
    case 0x2E: wL(g, fetch8(g)); return 8;
    case 0x2F: wA(g, (uint8_t)~rA(g)); wF(g, (uint8_t)(rF(g) | N_FLAG | H_FLAG)); return 4;
    case 0x30: e = (int8_t)fetch8(g); if (cond(g, 2)) { g->pc = (uint16_t)(g->pc + e); return 12; } return 8;
    case 0x31: g->sp = fetch16(g); return 12;
    case 0x32: gb_write(g, g->hl--, rA(g)); return 8;
    case 0x33: g->sp++; return 8;
    case 0x34: gb_write(g, g->hl, inc8(g, gb_read(g, g->hl))); return 12;
    case 0x35: gb_write(g, g->hl, dec8(g, gb_read(g, g->hl))); return 12;
    case 0x36: gb_write(g, g->hl, fetch8(g)); return 12;
    case 0x37: wF(g, (uint8_t)((rF(g) & Z_FLAG) | C_FLAG)); return 4;
    case 0x38: e = (int8_t)fetch8(g); if (cond(g, 3)) { g->pc = (uint16_t)(g->pc + e); return 12; } return 8;
    case 0x39: add_hl(g, g->sp); return 8;
    case 0x3A: wA(g, gb_read(g, g->hl--)); return 8;
    case 0x3B: g->sp--; return 8;
    case 0x3C: wA(g, inc8(g, rA(g))); return 4;
    case 0x3D: wA(g, dec8(g, rA(g))); return 4;
    case 0x3E: wA(g, fetch8(g)); return 8;
    case 0x3F: wF(g, (uint8_t)((rF(g) & Z_FLAG) | ((rF(g) & C_FLAG) ? 0 : C_FLAG))); return 4;
    case 0xC0: if (cond(g, 0)) { g->pc = pop16(g); return 20; } return 8;
    case 0xC1: g->bc = pop16(g); return 12;
    case 0xC2: nn = fetch16(g); if (cond(g, 0)) { g->pc = nn; return 16; } return 12;
    case 0xC3: g->pc = fetch16(g); return 16;
    case 0xC4: nn = fetch16(g); if (cond(g, 0)) { push16(g, g->pc); g->pc = nn; return 24; } return 12;
    case 0xC5: push16(g, g->bc); return 16;
    case 0xC6: alu(g, 0, fetch8(g)); return 8;
    case 0xC7: push16(g, g->pc); g->pc = 0x00; return 16;
    case 0xC8: if (cond(g, 1)) { g->pc = pop16(g); return 20; } return 8;
    case 0xC9: g->pc = pop16(g); return 16;
    case 0xCA: nn = fetch16(g); if (cond(g, 1)) { g->pc = nn; return 16; } return 12;
    case 0xCC: nn = fetch16(g); if (cond(g, 1)) { push16(g, g->pc); g->pc = nn; return 24; } return 12;
    case 0xCD: nn = fetch16(g); push16(g, g->pc); g->pc = nn; return 24;
    case 0xCE: alu(g, 1, fetch8(g)); return 8;
    case 0xCF: push16(g, g->pc); g->pc = 0x08; return 16;
    case 0xD0: if (cond(g, 2)) { g->pc = pop16(g); return 20; } return 8;
    case 0xD1: g->de = pop16(g); return 12;
    case 0xD2: nn = fetch16(g); if (cond(g, 2)) { g->pc = nn; return 16; } return 12;
    case 0xD4: nn = fetch16(g); if (cond(g, 2)) { push16(g, g->pc); g->pc = nn; return 24; } return 12;
    case 0xD5: push16(g, g->de); return 16;
    case 0xD6: alu(g, 2, fetch8(g)); return 8;
    case 0xD7: push16(g, g->pc); g->pc = 0x10; return 16;
    case 0xD8: if (cond(g, 3)) { g->pc = pop16(g); return 20; } return 8;
    case 0xD9: g->pc = pop16(g); g->ime = 1; return 16;
    case 0xDA: nn = fetch16(g); if (cond(g, 3)) { g->pc = nn; return 16; } return 12;
    case 0xDC: nn = fetch16(g); if (cond(g, 3)) { push16(g, g->pc); g->pc = nn; return 24; } return 12;
    case 0xDE: alu(g, 3, fetch8(g)); return 8;
    case 0xDF: push16(g, g->pc); g->pc = 0x18; return 16;
    case 0xE0: gb_write(g, (uint16_t)(0xFF00 + fetch8(g)), rA(g)); return 12;
    case 0xE1: g->hl = pop16(g); return 12;
    case 0xE2: gb_write(g, (uint16_t)(0xFF00 + rC(g)), rA(g)); return 8;
    case 0xE5: push16(g, g->hl); return 16;
    case 0xE6: alu(g, 4, fetch8(g)); return 8;
    case 0xE7: push16(g, g->pc); g->pc = 0x20; return 16;
    case 0xE8: {
        e = (int8_t)fetch8(g);
        unsigned r = g->sp + e;
        uint8_t f = 0;
        if ((g->sp & 0xFF) + (e & 0xFF) > 0xFF) f |= C_FLAG;
        if ((g->sp & 0x0F) + (e & 0x0F) > 0x0F) f |= H_FLAG;
        wF(g, f);
        g->sp = (uint16_t)r;
        return 16;
    }
    case 0xE9: g->pc = g->hl; return 4;
    case 0xEA: gb_write(g, fetch16(g), rA(g)); return 16;
    case 0xEE: alu(g, 5, fetch8(g)); return 8;
    case 0xEF: push16(g, g->pc); g->pc = 0x28; return 16;
    case 0xF0: wA(g, gb_read(g, (uint16_t)(0xFF00 + fetch8(g)))); return 12;
    case 0xF1: { uint16_t v = pop16(g); g->af = (uint16_t)((v & 0xFFF0)); return 12; }
    case 0xF2: wA(g, gb_read(g, (uint16_t)(0xFF00 + rC(g)))); return 8;
    case 0xF3: g->ime = 0; g->ime_sched = 0; return 4;
    case 0xF5: push16(g, g->af); return 16;
    case 0xF6: alu(g, 6, fetch8(g)); return 8;
    case 0xF7: push16(g, g->pc); g->pc = 0x30; return 16;
    case 0xF8: {
        e = (int8_t)fetch8(g);
        unsigned r = g->sp + e;
        uint8_t f = 0;
        if ((g->sp & 0xFF) + (e & 0xFF) > 0xFF) f |= C_FLAG;
        if ((g->sp & 0x0F) + (e & 0x0F) > 0x0F) f |= H_FLAG;
        wF(g, f);
        g->hl = (uint16_t)r;
        return 12;
    }
    case 0xF9: g->sp = g->hl; return 8;
    case 0xFA: wA(g, gb_read(g, fetch16(g))); return 16;
    case 0xFB: g->ime_sched = 1; return 4;
    case 0xFE: alu(g, 7, fetch8(g)); return 8;
    case 0xFF: push16(g, g->pc); g->pc = 0x38; return 16;
    default: return 4;
    }
}

static int gb_irq(gb_t *g) {
    uint8_t hit = (uint8_t)(g->io[0x0F] & g->ie & 0x1F);
    int bit = 0;
    while (bit < 5 && !(hit & (1 << bit))) bit++;
    g->ime = 0;
    g->halt = 0;
    g->io[0x0F] = (uint8_t)(g->io[0x0F] & ~(1 << bit));
    push16(g, g->pc);
    g->pc = (uint16_t)(0x40 + bit * 8);
    return 20;
}

static inline int gb_cpu_step(gb_t *g) {
    if (g->ime && (g->io[0x0F] & g->ie & 0x1F)) return gb_irq(g);
    if (g->halt) {
        if (g->io[0x0F] & g->ie & 0x1F) g->halt = 0;
        else return 4;
    }
    uint8_t sched = g->ime_sched;
    g->ime_sched = 0;
    int cy = gb_exec(g);
    if (sched) g->ime = 1;
    return cy;
}

static void gb_run_frame(gb_t *g) {
    g->frame = 0;
    rom_sram_copies = 3;
    int acc = 0;
    int guard = 0;
    while (!g->frame && guard++ < 400000) {
        acc += gb_cpu_step(g);
        if (acc >= 16) {
            gb_timer_step(g, acc);
            apu_step(g, acc);
            gb_ppu_step(g, acc);
            acc = 0;
        }
    }
    if (acc) {
        gb_timer_step(g, acc);
        apu_step(g, acc);
        gb_ppu_step(g, acc);
    }
}

static int mbc_of(uint8_t t) {
    if (t == 0x00 || t == 0x08 || t == 0x09) return MBC_NONE;
    if (t <= 0x03) return MBC_1;
    if (t == 0x05 || t == 0x06) return MBC_2;
    if (t >= 0x0F && t <= 0x13) return MBC_3;
    if (t >= 0x19 && t <= 0x1E) return MBC_5;
    return -1;
}

static uint32_t cram_bytes(uint8_t code, int mbc) {
    if (mbc == MBC_2) return 512;
    static const uint32_t sz[] = {0, 0x800, 0x2000, 0x8000, 0x20000, 0x10000};
    if (code >= sizeof sz / sizeof sz[0]) return 0;
    return sz[code];
}

static void gb_reset(gb_t *g) {
    memset(g->wram, 0, sizeof g->wram);
    memset(g->vram, 0, sizeof g->vram);
    memset(g->oam, 0, sizeof g->oam);
    memset(g->io, 0, sizeof g->io);
    memset(g->hram, 0, sizeof g->hram);
    memset(g->bg_pal, 0xFF, sizeof g->bg_pal);
    memset(g->ob_pal, 0xFF, sizeof g->ob_pal);
    g->af = g->cgb ? 0x1180 : 0x01B0;
    g->bc = 0x0013;
    g->de = 0x00D8;
    g->hl = 0x014D;
    g->sp = 0xFFFE;
    g->pc = 0x0100;
    g->ime = 0;
    g->ime_sched = 0;
    g->halt = 0;
    g->rom_bank = 1;
    g->ram_bank = 0;
    g->ram_en = 0;
    g->mbc_mode = 0;
    g->vbk = 0;
    g->svbk = 1;
    g->hdma_len = -1;
    g->io[0x00] = 0xCF;
    g->io[0x0F] = 0xE1;
    g->io[0x40] = 0x91;
    g->io[0x41] = 0x85;
    g->io[0x47] = 0xFC;
    g->io[0x48] = 0xFF;
    g->io[0x49] = 0xFF;
    g->io[0x4D] = 0;
    g->ppu_mode = 2;
    g->ppu_len = 80;
    g->ppu_dots = 0;
    g->win_line = 0;
    g->double_speed = 0;
    g->speed_armed = 0;
    g->div_cc = 0xABCC;
    g->ie = 0;
    apu_reset(g);
    mem_map(g);
    pal_rebuild(g);
    for (int i = 0; i < 4; i++)
        dmg_565[i] = rgb_to_565be(dmg_rgb[i][0], dmg_rgb[i][1], dmg_rgb[i][2]);
    for (int i = 0; i < 8; i++) rom_slot_off[i] = ~0u;
    rom_sram_copies = 8;
    rom_cache_sync(g);
}

static void sav_path(const char *rom_path, char *out, int max) {
    const char *base = strrchr(rom_path, '/');
    base = base ? base + 1 : rom_path;
    snprintf(out, (size_t)max, "EMU/SAVES/%s", base);
    char *dot = strrchr(out, '.');
    if (dot) strcpy(dot, ".sav");
    else strncat(out, ".sav", (size_t)max - strlen(out) - 1);
}

static void ui_msg(const char *title, const char *a, const char *b) {
    int cx, cy, cw, ch;
    os_gem_desktop_bg();
    os_window(title, &cx, &cy, &cw, &ch);
    gfx_puts_fit(cx + 4, cy + 24, a, GEM_BLACK, GEM_WHITE, cw - 8);
    if (b) gfx_puts_fit(cx + 4, cy + 40, b, GEM_DGRAY, GEM_WHITE, cw - 8);
    gfx_puts_fit(cx + 4, cy + ch - 10, "any key", GEM_DGRAY, GEM_WHITE, cw - 8);
    gfx_flush();
    kbd_event_t ev;
    for (;;) {
        kbd_poll();
        if (kbd_get_event(&ev) && ev.type == KBD_EV_PRESS) break;
        sleep_ms(4);
    }
}

static void load_bar(const char *msg, int pct) {
    int cx, cy, cw, ch;
    os_gem_desktop_bg();
    os_window("EMU", &cx, &cy, &cw, &ch);
    gfx_puts_fit(cx + 4, cy + 24, msg, GEM_BLACK, GEM_WHITE, cw - 8);
    int bw = cw - 16;
    if (bw < 40) bw = 40;
    gfx_rect(cx + 8, cy + 48, bw, 12, GEM_BLACK);
    int fill = (bw - 2) * (pct < 0 ? 0 : pct > 100 ? 100 : pct) / 100;
    gfx_fill_rect(cx + 9, cy + 49, fill, 10, GEM_GREEN);
    gfx_flush();
}

static int apply_joy(gb_t *g, int code, int down) {
    uint8_t bit = 0;
    switch (code) {
    case KEY_RIGHT: case 'd': case 'D': case KEY_JOY_RIGHT: bit = 0x10; break;
    case KEY_LEFT:  case 'a': case 'A': case KEY_JOY_LEFT:  bit = 0x20; break;
    case KEY_UP:    case 'w': case 'W': case KEY_JOY_UP:    bit = 0x40; break;
    case KEY_DOWN:  case 's': case 'S': case KEY_JOY_DOWN:  bit = 0x80; break;
    case 'z': case 'Z': case ' ': case KEY_JOY_CENTER: case KEY_BTN_RIGHT1: bit = 0x01; break;
    case 'x': case 'X': case KEY_BTN_LEFT1: bit = 0x02; break;
    case KEY_TAB: bit = 0x04; break;
    case KEY_ENTER: bit = 0x08; break;
    default: return 0;
    }
    if (down) {
        if (!(g->joy & bit)) g->io[0x0F] |= 0x10;
        g->joy |= bit;
    } else g->joy &= (uint8_t)~bit;
    return 1;
}

#if defined(ESP_PLATFORM)
#define ROM_HDR 4096
typedef struct {
    char magic[8];
    char name[64];
    uint32_t size;
    uint32_t pad;
} rom_hdr_t;

static uint8_t *flash_rom(const char *name, uint32_t size) {
    const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "romcache");
    if (!p || p->size < size + ROM_HDR) return NULL;
    rom_hdr_t hdr;
    if (esp_partition_read(p, 0, &hdr, sizeof hdr) == ESP_OK &&
        memcmp(hdr.magic, "PREMU1", 6) == 0 && hdr.size == size &&
        strncmp(hdr.name, name, 63) == 0) {
        const void *ptr = NULL;
        uint32_t map = (size + ROM_HDR + 65535u) & ~65535u;
        if (map > p->size) map = p->size;
        if (esp_partition_mmap(p, 0, map, SPI_FLASH_MMAP_DATA, &ptr, &rom_mmap) == ESP_OK) {
            rom_mmap_on = 1;
            return (uint8_t *)ptr + ROM_HDR;
        }
    }
    uint32_t erase = (size + ROM_HDR + 4095u) & ~4095u;
    if (erase > p->size) erase = p->size;
    if (rom_mmap_on) { spi_flash_munmap(rom_mmap); rom_mmap_on = 0; }
    load_bar("Erasing flash cache...", 0);
    if (esp_partition_erase_range(p, 0, erase) != ESP_OK) return NULL;
    uint8_t buf[4096];
    uint32_t off = 0;
    sdfs_ro_seek(0);
    while (off < size) {
        uint32_t n = size - off;
        if (n > sizeof buf) n = sizeof buf;
        memset(buf, 0xFF, sizeof buf);
        int got = sdfs_ro_read(buf, n);
        if (got < 0 || (uint32_t)got != n) return NULL;
        if (esp_partition_write(p, ROM_HDR + off, buf, sizeof buf) != ESP_OK) return NULL;
        off += sizeof buf;
        uint32_t shown = off > size ? size : off;
        load_bar("Copying ROM to flash...", (int)(shown * 100u / size));
    }
    memset(&hdr, 0, sizeof hdr);
    memcpy(hdr.magic, "PREMU1", 6);
    strncpy(hdr.name, name, 63);
    hdr.size = size;
    if (esp_partition_write(p, 0, &hdr, sizeof hdr) != ESP_OK) return NULL;
    const void *ptr = NULL;
    uint32_t map = (size + ROM_HDR + 65535u) & ~65535u;
    if (map > p->size) map = p->size;
    if (esp_partition_mmap(p, 0, map, SPI_FLASH_MMAP_DATA, &ptr, &rom_mmap) != ESP_OK)
        return NULL;
    rom_mmap_on = 1;
    return (uint8_t *)ptr + ROM_HDR;
}
#endif

static void *xalloc(size_t n) {
#if defined(ESP_PLATFORM)
    return heap_caps_calloc(1, n, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
#else
    return calloc(1, n);
#endif
}

static size_t largest_free(void) {
#if defined(ESP_PLATFORM)
    return heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
#else
    return 256 * 1024;
#endif
}

void emu_gb_run(const char *path) {
    if (!path || !sdfs_open_ro(path)) {
        ui_msg("EMU", "Cannot open ROM", path);
        return;
    }
    uint32_t size = sdfs_ro_size();
    if (size < 0x150) {
        sdfs_close_ro();
        ui_msg("EMU", "ROM too small", NULL);
        return;
    }
    load_bar("Reading header...", 0);
    uint8_t hdr[0x150];
    if (sdfs_ro_read(hdr, sizeof hdr) != (int)sizeof hdr) {
        sdfs_close_ro();
        ui_msg("EMU", "ROM read failed", NULL);
        return;
    }
    int mbc = mbc_of(hdr[0x147]);
    if (mbc < 0) {
        sdfs_close_ro();
        ui_msg("EMU", "Unsupported cartridge type", NULL);
        return;
    }
    uint32_t cram = cram_bytes(hdr[0x149], mbc);
    int cgb = (hdr[0x143] & 0x80) != 0;
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;

    size_t room = largest_free();
    uint8_t *rom = NULL;
    int rom_owned = 0;
    if (room > size + 24 * 1024u) {
        load_bar("Loading ROM...", 5);
#if defined(ESP_PLATFORM)
        rom = (uint8_t *)heap_caps_malloc(size, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
#else
        rom = (uint8_t *)malloc(size);
#endif
        if (rom) {
            memcpy(rom, hdr, sizeof hdr);
            uint32_t off = sizeof hdr;
            while (off < size) {
                uint32_t n = size - off;
                if (n > 4096) n = 4096;
                int got = sdfs_ro_read(rom + off, n);
                if (got < 0 || (uint32_t)got != n) { free(rom); rom = NULL; break; }
                off += n;
                load_bar("Loading ROM...", (int)(off * 100u / size));
            }
            if (rom) rom_owned = 1;
        }
    }
#if defined(ESP_PLATFORM)
    if (!rom) {
        load_bar("Copying ROM to flash cache...", 0);
        rom = flash_rom(base, size);
        rom_owned = 0;
    }
#endif
    sdfs_close_ro();
    if (!rom) {
        ui_msg("EMU", "ROM too large for RAM", "Need flash cache partition");
        return;
    }

    int core_in_fb = 0;
    gb_t *g = NULL;
    uint8_t *cram_ptr = NULL;
#if defined(ESP_PLATFORM)
    {
        size_t need = sizeof(gb_t) + (size_t)cram;
        size_t fb_off = 16u * (size_t)LCD_WIDTH;
        size_t fb_sz = (size_t)LCD_WIDTH * (size_t)LCD_HEIGHT;
        if (gfx_fb && fb_off + need <= fb_sz) {
            g = (gb_t *)(gfx_fb + fb_off);
            memset(g, 0, sizeof(*g));
            if (cram) {
                cram_ptr = gfx_fb + fb_off + sizeof(gb_t);
                memset(cram_ptr, 0, cram);
            }
            core_in_fb = 1;
        }
    }
#endif
    if (!g) {
        g = (gb_t *)xalloc(sizeof(gb_t));
        if (g && cram) {
            cram_ptr = (uint8_t *)xalloc(cram);
            if (!cram_ptr) { free(g); g = NULL; }
        }
    }
    if (!g) {
        if (rom_owned) free(rom);
#if defined(ESP_PLATFORM)
        if (rom_mmap_on) { spi_flash_munmap(rom_mmap); rom_mmap_on = 0; }
#endif
        ui_msg("EMU", "Out of RAM (core)", NULL);
        return;
    }
    g->mbc = (uint8_t)mbc;
    g->cgb = (uint8_t)cgb;
    g->rom_size = size;
    g->cram_size = cram;
    g->rom_banks = size / 0x4000u;
    if (!g->rom_banks) g->rom_banks = 1;
    g->rom = rom;
    g->rom_owned = (uint8_t)rom_owned;
    g->cram = cram_ptr;
    rom_cache_alloc(g);

    if (cram && g->cram) {
        char sp[80];
        sav_path(path, sp, (int)sizeof sp);
        if (sdfs_open_ro(sp)) {
            uint32_t n = sdfs_ro_size();
            if (n > cram) n = cram;
            sdfs_ro_read(g->cram, n);
            sdfs_close_ro();
        }
    }
    gb_reset(g);

    gfx_fill_rect(0, 0, LCD_WIDTH, 16, COL_BLACK);
    gfx_fill_rect(0, 304, LCD_WIDTH, 16, COL_BLACK);
    gfx_puts_at(4, 4, cgb ? "GBC" : "GB", COL_LGREEN, COL_BLACK);
    gfx_puts_fit(36, 4, base, COL_WHITE, COL_BLACK, 200);
    gfx_puts_at(248, 4, "ESC=quit", COL_GRAY, COL_BLACK);
    gfx_flush();

    sound_pcm_start(SOUND_PCM_RATE);
    g->joy = 0;
#if defined(ESP_PLATFORM)
#define emu_now_us() esp_timer_get_time()
#else
#define emu_now_us() ((int64_t)to_us_since_boot(get_absolute_time()))
#endif
    int64_t origin = emu_now_us();
    int64_t emu_us = 0;
    int paused = 0, poll_div = 0, present_i = 0;
    const int64_t FRAME_US = 16743;
    for (;;) {
        if (paused || ((++poll_div) & 1) == 0) kbd_poll();
        kbd_event_t ev;
        int quit = 0;
        while (kbd_get_event(&ev)) {
            if (ev.code == KEY_ESC && ev.type == KBD_EV_PRESS) { quit = 1; break; }
            if (ev.code == KEY_F1 && ev.type == KBD_EV_PRESS) {
                paused = !paused;
                if (paused) {
                    sound_pcm_silence();
                    gfx_direct_end();
                    gfx_puts_at(4, 308, "PAUSED  F1=resume  ESC=quit", COL_YELLOW, COL_BLACK);
                    gfx_flush();
                }
            }
            apply_joy(g, ev.code, ev.type == KBD_EV_PRESS);
        }
        if (quit) break;
        if (paused) { sleep_ms(16); continue; }

        /* Every third emulated frame (~20 Hz). Do not key off wall clock:
         * that drew every frame when behind and made slowdown worse. */
        g->present = (++present_i % 3) == 0;
        gb_run_frame(g);
        emu_us += FRAME_US;
#if defined(ESP_PLATFORM)
        esp_task_wdt_reset();
        taskYIELD();
#endif
        int64_t target = origin + emu_us;
        int64_t now = emu_now_us();
        int64_t ahead = target - now;
        if (ahead > 2000) sleep_ms((uint32_t)(ahead / 1000));
        else if (ahead > 200) sleep_us((uint32_t)ahead);
        else if (ahead < -80000) origin = now - emu_us;
    }
    gfx_direct_end();
    sound_pcm_stop();

    if (g->cram && g->cram_size) {
        char sp[80];
        sav_path(path, sp, (int)sizeof sp);
        sdfs_mkdir("EMU/SAVES");
        sdfs_write_file(sp, (char *)g->cram, g->cram_size);
    }
#if defined(ESP_PLATFORM)
    if (rom_mmap_on) {
        spi_flash_munmap(rom_mmap);
        rom_mmap_on = 0;
    }
#endif
    rom_cache_free();
    if (g->rom_owned) free(g->rom);
    if (!core_in_fb) {
        free(g->cram);
        free(g);
    }
}
