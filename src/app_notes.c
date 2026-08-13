/* PRetroCalc OS — NOTES app
 * UTF-8 markdown notes with a 1:1 Aksara Jawa (Carakan) keyboard layer.
 * Files live under NOTES/*.md on the SD card. */
#include "apps.h"
#include "os.h"
#include "gfx.h"
#include "font.h"
#include "font_jawa.h"
#include "keyboard.h"
#include "sound.h"
#include "sdfs.h"
#include "board.h"
#include "pico/stdlib.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#define NT_MAX   6144
#define NT_ROWS  8
#define NT_PATH  "NOTES"
#define STYLE_BOLD     1
#define STYLE_ITAL     2
#define STYLE_H1       4
#define STYLE_H2       8
#define STYLE_H3       16
#define STYLE_LIST     32
#define STYLE_JAWA     64    /* line contains Aksara Jawa — use full stack height */
#define STYLE_PASANGAN 128   /* subscript consonant (after pangkon) */

#define LATIN_SCALE 2                 /* Latin rendered at 2x for readability */
#define LATIN_W     (FONT_W * LATIN_SCALE)
#define LATIN_H     (FONT_H * LATIN_SCALE)

#define JAWA_PANGKON 0xA9C0u

static int gx, gy, gw, gh;

static void draw_frame(const char *title) {
    os_gem_desktop_bg();
    os_window(title, &gx, &gy, &gw, &gh);
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

/* ---------- UTF-8 ---------- */

static int utf8_decode(const char *s, int len, uint32_t *cp) {
    if (len <= 0) return 0;
    const unsigned char *u = (const unsigned char *)s;
    unsigned char c = u[0];
    if (c < 0x80) { *cp = c; return 1; }
    if ((c & 0xE0) == 0xC0 && len >= 2) {
        *cp = ((c & 0x1F) << 6) | (u[1] & 0x3F);
        return 2;
    }
    if ((c & 0xF0) == 0xE0 && len >= 3) {
        *cp = ((c & 0x0F) << 12) | ((u[1] & 0x3F) << 6) | (u[2] & 0x3F);
        return 3;
    }
    if ((c & 0xF8) == 0xF0 && len >= 4) {
        *cp = ((c & 0x07) << 18) | ((u[1] & 0x3F) << 12) |
              ((u[2] & 0x3F) << 6) | (u[3] & 0x3F);
        return 4;
    }
    *cp = 0xFFFD;
    return 1;
}

static int utf8_encode(uint32_t cp, char out[4]) {
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

static int utf8_prev(const char *buf, int pos) {
    if (pos <= 0) return 0;
    pos--;
    while (pos > 0 && ((unsigned char)buf[pos] & 0xC0) == 0x80) pos--;
    return pos;
}

static int utf8_next(const char *buf, int len, int pos) {
    if (pos >= len) return len;
    uint32_t cp;
    int n = utf8_decode(buf + pos, len - pos, &cp);
    return pos + (n > 0 ? n : 1);
}

/* ---------- Carakan keyboard (1 key = 1 aksara, no romanization) ----------
 *
 * Number row  → sandhangan / pangkon (marks)
 * Top letter  → ha na ca ra ka | da ta sa wa la
 * Home row    → pa dha ja ya nya | ma ga ba tha nga
 * Bottom row  → independent vowels + pada punctuation
 * Shift       → murda / mahaprana / Jawa digits / extra marks
 */

static uint32_t jawa_map_key(int c, bool shift) {
    /* sandhangan on digit row — pure 1:1, not "type the vowel letter" */
    if (!shift) {
        switch (c) {
        case '1': return 0xA9B6; /* wulu */
        case '2': return 0xA9B8; /* suku */
        case '3': return 0xA9BA; /* taling (type AFTER aksara; draws to the left) */
        case '4': return 0xA9B4; /* tarung */
        case '5': return 0xA9BC; /* pepet */
        case '6': return 0xA9BF; /* cakra */
        case '7': return 0xA9BE; /* pengkal */
        case '8': return 0xA9BD; /* keret */
        case '9': return 0xA981; /* cecak */
        case '0': return 0xA982; /* layar */
        case '-': return 0xA983; /* wignyan */
        case '=': return 0xA9C0; /* pangkon */
        /* carakan row 1 — lowercase only; uppercase is murda via Alt layer */
        case 'q': return 0xA9B2; /* ha */
        case 'w': return 0xA9A4; /* na */
        case 'e': return 0xA995; /* ca */
        case 'r': return 0xA9AB; /* ra */
        case 't': return 0xA98F; /* ka */
        case 'y': return 0xA9A2; /* da */
        case 'u': return 0xA9A0; /* ta */
        case 'i': return 0xA9B1; /* sa */
        case 'o': return 0xA9AE; /* wa */
        case 'p': return 0xA9AD; /* la */
        /* carakan row 2 */
        case 'a': return 0xA9A5; /* pa */
        case 's': return 0xA99D; /* dha */
        case 'd': return 0xA997; /* ja */
        case 'f': return 0xA9AA; /* ya */
        case 'g': return 0xA99A; /* nya */
        case 'h': return 0xA9A9; /* ma */
        case 'j': return 0xA992; /* ga */
        case 'k': return 0xA9A7; /* ba */
        case 'l': return 0xA99B; /* tha */
        case ';': return 0xA994; /* nga */
        /* independent vowels + pada */
        case 'z': return 0xA984; /* A */
        case 'x': return 0xA986; /* I */
        case 'c': return 0xA988; /* U */
        case 'v': return 0xA98C; /* E */
        case 'b': return 0xA98E; /* O */
        case 'n': return 0xA9CA; /* pada adeg */
        case 'm': return 0xA9CB; /* pada adeg-adeg */
        case ',': return 0xA9C8; /* lingsa */
        case '.': return 0xA9C9; /* lungsi */
        case '/': return 0xA9CF; /* pangrangkep */
        case ' ': return ' ';
        default: return 0;
        }
    }
    /* Shift layer: murda / mahaprana / digits / extra (also used by Alt+letter) */
    switch (c) {
    case '1': return 0xA9D1;
    case '2': return 0xA9D2;
    case '3': return 0xA9D3;
    case '4': return 0xA9D4;
    case '5': return 0xA9D5;
    case '6': return 0xA9D6;
    case '7': return 0xA9D7;
    case '8': return 0xA9D8;
    case '9': return 0xA9D9;
    case '0': return 0xA9D0;
    case '-': return 0xA9B3; /* cecak telu */
    case '=': return 0xA980; /* panyangga */
    case 'q': return 0xA9B2; /* ha (no murda) */
    case 'w': return 0xA99F; /* na murda */
    case 'e': return 0xA996; /* ca murda */
    case 'r': return 0xA9AC; /* ra agung */
    case 't': return 0xA991; /* ka murda */
    case 'y': return 0xA9A3; /* da mahaprana */
    case 'u': return 0xA9A1; /* ta murda */
    case 'i': return 0xA9AF; /* sa murda */
    case 'o': return 0xA9AE;
    case 'p': return 0xA9AD;
    case 'a': return 0xA9A6; /* pa murda */
    case 's': return 0xA99E; /* dha mahaprana */
    case 'd': return 0xA999; /* ja mahaprana */
    case 'f': return 0xA9AA;
    case 'g': return 0xA998; /* nya murda */
    case 'h': return 0xA9A9;
    case 'j': return 0xA993; /* ga murda */
    case 'k': return 0xA9A8; /* ba murda */
    case 'l': return 0xA99C; /* tha mahaprana */
    case ';': return 0xA994;
    case 'z': return 0xA985; /* i kawi */
    case 'x': return 0xA987; /* ii */
    case 'c': return 0xA989; /* pa cerek */
    case 'v': return 0xA98D; /* ai */
    case 'b': return 0xA98A; /* nga lelet */
    case 'n': return 0xA9C1; /* left rerenggan */
    case 'm': return 0xA9C2; /* right rerenggan */
    case ',': return 0xA9C7; /* pangkat */
    case '.': return 0xA9C6; /* windu */
    case '/': return 0xA9CC; /* piseleh */
    case ' ': return ' ';
    default: return 0;
    }
}

/* Alt(+Shift) extras — completes the Unicode block for typing. */
static uint32_t jawa_map_alt(int c, bool shift) {
    if (shift) {
        switch (c) {
        case 't': return 0xA990; /* ka sasak */
        case 'i': return 0xA9B0; /* sa mahaprana */
        case 'b': return 0xA98B; /* nga lelet raswadi */
        case '-': return 0xA9DE; /* tirta tumetes */
        case '=': return 0xA9DF; /* isukki tunggak */
        default: return 0;
        }
    }
    switch (c) {
    case '1': return 0xA9B7; /* wulu melik */
    case '2': return 0xA9B9; /* suku mendut */
    case '3': return 0xA9BB; /* dirga mure */
    case '4': return 0xA9B5; /* tolong */
    case '5': return 0xA9C3; /* pada andap */
    case '6': return 0xA9C4; /* pada madya */
    case '7': return 0xA9C5; /* pada luhur */
    case '8': return 0xA9CD; /* turned piseleh */
    case '0': return 0xA9CA; /* pada adeg (also on N) */
    case ',': return 0xA9C3; /* pada andap */
    case '.': return 0xA9C4; /* pada madya */
    case '/': return 0xA9C5; /* pada luhur */
    default: return 0;
    }
}

/* ---------- note buffer ---------- */

static char nt_buf[NT_MAX];
static int nt_len;
static int nt_cur;
static char nt_file[48];
static bool nt_dirty;
static bool nt_jawa;   /* Jawa keyboard layer on */
static bool nt_raw;    /* show markdown source */
static int  nt_hscroll; /* horizontal pixel scroll (no wrap) */
static int  nt_content_max_x; /* rightmost logical x of laid-out content */
static int  nt_view_left;     /* content left edge (after arrow gutter) */
static int  nt_view_right;    /* content right edge */

#define SCRIPT_GAP 6   /* extra px between Latin ↔ Jawa runs */

static void nt_ensure_dir(void) {
    sdfs_mkdir(NT_PATH);
}

static void nt_insert_bytes(const char *bytes, int n) {
    if (n <= 0 || nt_len + n >= NT_MAX) return;
    memmove(nt_buf + nt_cur + n, nt_buf + nt_cur, nt_len - nt_cur);
    memcpy(nt_buf + nt_cur, bytes, n);
    nt_cur += n;
    nt_len += n;
    nt_buf[nt_len] = 0;
    nt_dirty = true;
}

static void nt_insert_cp(uint32_t cp) {
    char tmp[4];
    int n = utf8_encode(cp, tmp);
    nt_insert_bytes(tmp, n);
}

static void nt_backspace(void) {
    if (nt_cur <= 0) return;
    int p = utf8_prev(nt_buf, nt_cur);
    int n = nt_cur - p;
    memmove(nt_buf + p, nt_buf + nt_cur, nt_len - nt_cur);
    nt_cur = p;
    nt_len -= n;
    nt_buf[nt_len] = 0;
    nt_dirty = true;
}

static void nt_delete(void) {
    if (nt_cur >= nt_len) return;
    int npos = utf8_next(nt_buf, nt_len, nt_cur);
    int n = npos - nt_cur;
    memmove(nt_buf + nt_cur, nt_buf + npos, nt_len - npos);
    nt_len -= n;
    nt_buf[nt_len] = 0;
    nt_dirty = true;
}

static void nt_wrap_md(const char *left, const char *right) {
    int ll = (int)strlen(left), rl = (int)strlen(right);
    if (nt_len + ll + rl >= NT_MAX) return;
    nt_insert_bytes(left, ll);
    int mid = nt_cur;
    nt_insert_bytes(right, rl);
    nt_cur = mid;
}

/* ---------- glyph lookup / draw ---------- */

static int cp_advance(uint32_t cp) {
    if (cp == '\n' || cp == 0) return 0;
    if (cp == 0x2022) return LATIN_W + 4;
    if (font_jawa_is_mark(cp)) return 0;
    if (font_jawa_is_base(cp) || font_jawa_glyph(cp)) return font_jawa_advance(cp);
    return LATIN_W;
}

/* Latin-only lines stay tight; any Jawa on the line → full stack box. */
static int line_height_for(int style) {
    int h = (style & STYLE_JAWA) ? (font_jawa_h() + 4) : (LATIN_H + 2);
    if (style & STYLE_H1) h += 8;
    else if (style & STYLE_H2) h += 4;
    return h;
}

/* True if this logical line (until \\n) contains any Javanese codepoint. */
static bool line_has_jawa(const char *s, int len) {
    int i = 0;
    while (i < len && s[i] != '\n') {
        uint32_t cp;
        int n = utf8_decode(s + i, len - i, &cp);
        if (n <= 0) break;
        if (font_jawa_glyph(cp)) return true;
        i += n;
    }
    return false;
}

static void draw_jawa_glyph(int x, int y, uint32_t cp, uint8_t fg, uint8_t bg, bool bold) {
    const uint8_t *g = font_jawa_glyph(cp);
    if (!g) return;
    int jw = font_jawa_w(), jh = font_jawa_h(), jrb = font_jawa_row_bytes();
    /* Always transparent bg so advance-based spacing isn't covered by a full em box */
    if (bg != 0xFF && bg != GEM_WHITE)
        gfx_fill_rect(x, y, jw, jh, bg);
    gfx_glyph_n(x, y, jw, jh, jrb, g, fg, 0xFF, bold);
}

/* Pasangan: real OpenType .pas glyph, same origin as base (ink lives in the
 * lower half of the em — do NOT half-scale nglegena, and do NOT lift it up
 * into the base body). */
static void draw_jawa_pasangan(int x, int y, uint32_t cp, uint8_t fg) {
    const uint8_t *g = font_jawa_pasangan(cp);
    if (!g) return;
    int jw = font_jawa_w(), jh = font_jawa_h(), jrb = font_jawa_row_bytes();
    gfx_glyph_n(x, y + font_jawa_pasangan_dy(), jw, jh, jrb, g, fg, 0xFF, false);
}

/* Below vowel attached to pasangan (u.ns.pas) — same em origin as base/pas. */
static void draw_jawa_pas_below(int x, int y, uint32_t cp, uint8_t fg) {
    const uint8_t *g = font_jawa_pas_below(cp);
    if (!g) return;
    int jw = font_jawa_w(), jh = font_jawa_h(), jrb = font_jawa_row_bytes();
    gfx_glyph_n(x, y, jw, jh, jrb, g, fg, 0xFF, false);
}

/* True if cp has a pasangan form (carakan / murda consonants). */
static bool jawa_can_pasangan(uint32_t cp) {
    return font_jawa_can_pasangan(cp);
}

static void draw_cp(int x, int y, uint32_t cp, uint8_t fg, uint8_t bg, int style, int lh) {
    bool bold = (style & (STYLE_BOLD | STYLE_H1 | STYLE_H2 | STYLE_H3)) != 0;
    bool ital = (style & STYLE_ITAL) != 0 && cp < 128;
    if (font_jawa_glyph(cp)) {
        draw_jawa_glyph(x, y, cp, fg, bg, bold);
        return;
    }
    if (cp >= 128) cp = '?';
    /* Latin: center vertically in the line box (tall when mixed with Jawa).
     * bg == 0xFF → transparent so H1 text on the green bar stays visible. */
    int ly = y + (lh - LATIN_H) / 2;
    if (ly < y) ly = y;
    gfx_glyph_scale(x, ly, (char)cp, LATIN_SCALE, fg, bg, bold, ital);
}

/* 0=other 1=Jawa 2=Latin — used for mixed-script gap */
static int script_class(uint32_t cp) {
    if (cp == 0 || cp == '\n' || cp == 0x2022) return 0;
    if (font_jawa_is_mark(cp) || font_jawa_is_base(cp) || font_jawa_glyph(cp)) return 1;
    if (cp < 128) return 2;
    return 0;
}

/* ---------- markdown-aware layout ---------- */

typedef struct {
    int byte_off;   /* buffer offset of this display cell */
    int x, y;       /* logical pixel pos (x may exceed screen — hscroll) */
    uint16_t cp;
    uint8_t style;
    uint8_t advance; /* pixels */
    uint8_t lh;      /* line box height for this cell */
} nt_cell_t;

#define NT_CELLS 768
static nt_cell_t nt_cells[NT_CELLS];
static int nt_ncells;
static int nt_cur_cell; /* cell index nearest cursor */

/* Find base aksara cell to attach sandhangan/pasangan to (same visual line). */
static int find_stack_base(void) {
    for (int i = nt_ncells - 1; i >= 0; i--) {
        if (nt_cells[i].cp == '\n') break;
        /* real base = consonant/vowel with advance, not a pasangan subscript */
        if (font_jawa_is_base(nt_cells[i].cp) && nt_cells[i].advance &&
            !(nt_cells[i].style & STYLE_PASANGAN))
            return i;
    }
    return -1;
}

/* True if a pasangan already shares this stack origin. */
static bool stack_has_pasangan(int x, int y) {
    /* Match by (x,y) only — do NOT stop at newlines. A backward scan that
     * breaks on '\\n' fails whenever any later line exists in nt_cells, which
     * made suku skip the under-pasangan shift and sit to the right. */
    for (int i = 0; i < nt_ncells; i++) {
        if (nt_cells[i].x == x && nt_cells[i].y == y &&
            (nt_cells[i].style & STYLE_PASANGAN))
            return true;
    }
    return false;
}

/* Raise stored line-box height for every cell on the current visual line. */
static void bump_line_lh(int y, int new_lh) {
    for (int i = 0; i < nt_ncells; i++) {
        if (nt_cells[i].y == y && nt_cells[i].lh < new_lh)
            nt_cells[i].lh = (uint8_t)new_lh;
    }
}

/* Was the most recently emitted visible mark a pangkon on this line? */
static bool last_cell_is_pangkon(void) {
    for (int i = nt_ncells - 1; i >= 0; i--) {
        if (nt_cells[i].cp == '\n') return false;
        if (nt_cells[i].cp == 0) continue; /* hidden markdown markers */
        return nt_cells[i].cp == JAWA_PANGKON;
    }
    return false;
}

static void emit_cell(int off, int x, int y, uint32_t cp, int style, int adv, int lh) {
    if (nt_ncells >= NT_CELLS) return;
    nt_cells[nt_ncells++] = (nt_cell_t){
        off, x, y, (uint16_t)cp,
        (uint8_t)style, (uint8_t)adv, (uint8_t)lh
    };
    if (x + adv > nt_content_max_x) nt_content_max_x = x + adv;
}

/* Emit pasangan under base if legal; returns true if consumed. */
static bool try_emit_pasangan(int pos, uint32_t cp, int style, int *lh) {
    if (!jawa_can_pasangan(cp) || !last_cell_is_pangkon()) return false;
    int bi = find_stack_base();
    if (bi < 0) return false;
    int bx = nt_cells[bi].x, by = nt_cells[bi].y;
    /* One pasangan per base — further consonants start a new syllable. */
    if (stack_has_pasangan(bx, by)) return false;
    if (nt_cur == pos) nt_cur_cell = nt_ncells;
    int plh = font_jawa_h() + 4;
    if (plh > *lh) {
        *lh = plh;
        bump_line_lh(by, plh);
    }
    emit_cell(pos, bx, by, cp, style | STYLE_PASANGAN, 0, *lh);
    return true;
}

/* Emit sandhangan on stack base if legal; returns true if consumed.
 * pen_x is the layout pen after the base — left/right marks may bump it. */
static bool try_emit_mark(int pos, uint32_t cp, int style, int *lh, int *pen_x) {
    if (!font_jawa_is_mark(cp)) return false;
    int bi = find_stack_base();
    if (bi < 0) return false;
    if (nt_cur == pos) nt_cur_cell = nt_ncells;
    int bx = nt_cells[bi].x, by = nt_cells[bi].y;
    int mlh = *lh;
    if (font_jawa_is_below_vowel(cp) && stack_has_pasangan(bx, by)) {
        mlh = font_jawa_h() + 4 + font_jawa_pasangan_extra();
        if (mlh > *lh) {
            *lh = mlh;
            bump_line_lh(by, mlh);
        }
    }
    /* Taling / dirga mure: typed AFTER the aksara (Unicode), drawn to the LEFT.
     * Shift the stack right so taling has room and doesn't clip the syllable. */
    if (font_jawa_is_left_mark(cp)) {
        int extra = font_jawa_left_mark_extra();
        for (int i = 0; i < nt_ncells; i++) {
            if (nt_cells[i].x == bx && nt_cells[i].y == by) {
                nt_cells[i].x += extra;
                if (nt_cells[i].advance) {
                    int r = nt_cells[i].x + nt_cells[i].advance;
                    if (r > nt_content_max_x) nt_content_max_x = r;
                }
            }
        }
        bx += extra;
        if (pen_x) *pen_x += extra;
    }
    /* Tarung / tolong stick out past ink-advance; grow the base so the next
     * aksara doesn't cover them. */
    if (font_jawa_is_right_mark(cp) && pen_x) {
        int extra = font_jawa_right_mark_extra();
        int adv = nt_cells[bi].advance + extra;
        if (adv > 255) adv = 255;
        nt_cells[bi].advance = (uint8_t)adv;
        *pen_x += extra;
        int r = nt_cells[bi].x + nt_cells[bi].advance;
        if (r > nt_content_max_x) nt_content_max_x = r;
    }
    emit_cell(pos, bx, by, cp, style, 0, mlh);
    return true;
}

static int match_prefix(const char *s, int len, const char *p) {
    int n = (int)strlen(p);
    if (len < n) return 0;
    for (int i = 0; i < n; i++) if (s[i] != p[i]) return 0;
    return n;
}

/* Parse ATX heading: #{1,3} optional space. Returns hide length, sets *st. */
static int match_heading(const char *s, int len, int *st) {
    int n = 0;
    while (n < 3 && n < len && s[n] == '#') n++;
    if (n == 0) return 0;
    /* require end, space, or start of content (GFM allows "#Title") */
    if (n < len && s[n] != ' ' && s[n] != '\t' && s[n] != '\n') {
        /* still a heading if only hashes+text */
    }
    if (n == 1) *st = STYLE_H1;
    else if (n == 2) *st = STYLE_H2;
    else *st = STYLE_H3;
    int hide = n;
    if (hide < len && (s[hide] == ' ' || s[hide] == '\t')) hide++;
    return hide;
}

/* Place next advancing glyph: no wrap, optional mixed-script gap. */
static int place_adv(int x, uint32_t cp, int *prev_cls) {
    int cls = script_class(cp);
    if (cls && *prev_cls && cls != *prev_cls) x += SCRIPT_GAP;
    if (cls) *prev_cls = cls;
    return x;
}

static void layout_note(int scroll_line, int max_y) {
    nt_ncells = 0;
    nt_cur_cell = -1;
    nt_content_max_x = 0;
    int pos = 0;
    int line = 0;
    while (pos < nt_len && line < scroll_line) {
        if (nt_buf[pos] == '\n') line++;
        pos++;
    }

    /* Logical x starts at 0; screen x = logical - hscroll + view_left */
    int x = 0;
    int y = gy + 2;
    int line_style = 0;
    int lh = line_height_for(0);
    bool fresh_line = true;
    int prev_cls = 0;

    while (pos <= nt_len && y < max_y && nt_ncells < NT_CELLS - 1) {
        if (pos == nt_len) {
            emit_cell(pos, x, y, 0, line_style, 0, lh);
            break;
        }

        if (fresh_line) {
            line_style = 0;
            prev_cls = 0;
            x = 0;
            if (line_has_jawa(nt_buf + pos, nt_len - pos))
                line_style |= STYLE_JAWA;
            lh = line_height_for(line_style);

            if (!nt_raw) {
                int rem = nt_len - pos;
                int hide = 0, m, st = 0;

                if ((hide = match_heading(nt_buf + pos, rem, &st))) {
                    line_style |= st;
                    lh = line_height_for(line_style);
                } else if ((m = match_prefix(nt_buf + pos, rem, "- ")) ||
                           (m = match_prefix(nt_buf + pos, rem, "* "))) {
                    line_style |= STYLE_LIST;
                    hide = m;
                    emit_cell(pos, x, y, 0x2022, line_style, LATIN_W + 4, lh);
                    x += LATIN_W + 4;
                    prev_cls = 2;
                } else if (rem >= 3 && nt_buf[pos] >= '0' && nt_buf[pos] <= '9' &&
                           nt_buf[pos + 1] == '.' && nt_buf[pos + 2] == ' ') {
                    line_style |= STYLE_LIST;
                    hide = 3;
                    emit_cell(pos, x, y, (uint8_t)nt_buf[pos], line_style, LATIN_W, lh);
                    x += LATIN_W;
                    emit_cell(pos + 1, x, y, '.', line_style, LATIN_W, lh);
                    x += LATIN_W + 4;
                    prev_cls = 2;
                }

                if (hide) {
                    for (int i = 0; i < hide && nt_ncells < NT_CELLS; i++)
                        emit_cell(pos + i, x, y, 0, line_style, 0, lh);
                    pos += hide;
                }
            }
            fresh_line = false;
            if (pos >= nt_len) {
                emit_cell(pos, x, y, 0, line_style, 0, lh);
                break;
            }
        }

        if (nt_buf[pos] == '\n') {
            if (nt_cur == pos) nt_cur_cell = nt_ncells;
            emit_cell(pos, x, y, '\n', line_style, 0, lh);
            pos++;
            x = 0;
            y += lh;
            line_style = 0;
            lh = line_height_for(0);
            fresh_line = true;
            prev_cls = 0;
            continue;
        }

        /* inline markdown — no wrap, keep going right */
        if (!nt_raw) {
            int rem = nt_len - pos;
            if (rem >= 2 && nt_buf[pos] == '*' && nt_buf[pos + 1] == '*') {
                int end = pos + 2;
                while (end + 1 < nt_len && !(nt_buf[end] == '*' && nt_buf[end + 1] == '*')) {
                    if (nt_buf[end] == '\n') break;
                    end++;
                }
                if (end + 1 < nt_len && nt_buf[end] == '*' && nt_buf[end + 1] == '*') {
                    int st = line_style | STYLE_BOLD;
                    emit_cell(pos, x, y, 0, st, 0, lh);
                    emit_cell(pos + 1, x, y, 0, st, 0, lh);
                    pos += 2;
                    while (pos < end && nt_ncells < NT_CELLS) {
                        uint32_t cp; int n = utf8_decode(nt_buf + pos, nt_len - pos, &cp);
                        int adv = cp_advance(cp);
                        if (try_emit_mark(pos, cp, st, &lh, &x)) { pos += n; continue; }
                        if (try_emit_pasangan(pos, cp, st, &lh)) { pos += n; continue; }
                        if (nt_cur == pos) nt_cur_cell = nt_ncells;
                        if (adv) x = place_adv(x, cp, &prev_cls);
                        emit_cell(pos, x, y, cp, st, adv, lh);
                        x += adv;
                        pos += n;
                    }
                    emit_cell(pos, x, y, 0, st, 0, lh);
                    emit_cell(pos + 1, x, y, 0, st, 0, lh);
                    pos += 2;
                    continue;
                }
            }
            char delim = 0;
            if (rem >= 1 && nt_buf[pos] == '*' && !(rem >= 2 && nt_buf[pos + 1] == '*')) delim = '*';
            else if (rem >= 1 && nt_buf[pos] == '_' && !(rem >= 2 && nt_buf[pos + 1] == '_')) delim = '_';
            if (delim) {
                int end = pos + 1;
                while (end < nt_len && nt_buf[end] != delim && nt_buf[end] != '\n') end++;
                if (end < nt_len && nt_buf[end] == delim) {
                    int st = line_style | STYLE_ITAL;
                    emit_cell(pos, x, y, 0, st, 0, lh);
                    pos++;
                    while (pos < end && nt_ncells < NT_CELLS) {
                        uint32_t cp; int n = utf8_decode(nt_buf + pos, nt_len - pos, &cp);
                        int adv = cp_advance(cp);
                        if (try_emit_mark(pos, cp, st, &lh, &x)) { pos += n; continue; }
                        if (try_emit_pasangan(pos, cp, st, &lh)) { pos += n; continue; }
                        if (nt_cur == pos) nt_cur_cell = nt_ncells;
                        if (adv) x = place_adv(x, cp, &prev_cls);
                        emit_cell(pos, x, y, cp, st, adv, lh);
                        x += adv;
                        pos += n;
                    }
                    emit_cell(pos, x, y, 0, st, 0, lh);
                    pos++;
                    continue;
                }
            }
        }

        uint32_t cp;
        int n = utf8_decode(nt_buf + pos, nt_len - pos, &cp);
        int adv = cp_advance(cp);

        if (try_emit_mark(pos, cp, line_style, &lh, &x)) { pos += n; continue; }
        if (try_emit_pasangan(pos, cp, line_style, &lh)) { pos += n; continue; }

        /* Orphan mark (no base): placeholder advance so it stays visible. */
        if (font_jawa_is_mark(cp)) {
            adv = font_jawa_advance(0xA98F);
            if (!adv) adv = font_jawa_w() / 2;
        }

        if (adv) x = place_adv(x, cp, &prev_cls);
        if (nt_cur == pos) nt_cur_cell = nt_ncells;
        emit_cell(pos, x, y, cp, line_style, adv, lh);
        x += adv;
        pos += n;
    }

    if (nt_cur_cell < 0) {
        for (int i = 0; i < nt_ncells; i++) {
            if (nt_cells[i].byte_off >= nt_cur) { nt_cur_cell = i; break; }
        }
        if (nt_cur_cell < 0 && nt_ncells > 0) nt_cur_cell = nt_ncells - 1;
    }
}

static void render_note(int scroll) {
    char title[40];
    snprintf(title, sizeof title, "NOTES%s", nt_dirty ? "*" : "");
    draw_frame(title);

    char st[48];
    if (nt_jawa)
        snprintf(st, sizeof st, "%s  JAWA%d%s",
                 nt_file[0] ? nt_file : "(new)",
                 font_jawa_px(),
                 nt_raw ? " RAW" : "");
    else
        snprintf(st, sizeof st, "%s  LATIN%s",
                 nt_file[0] ? nt_file : "(new)",
                 nt_raw ? " RAW" : "");
    gfx_puts_at(gx + 2, gy + gh - 20, st, GEM_DGRAY, GEM_WHITE);
    gfx_puts_at(gx + 2, gy + gh - 10,
                "F1=map F3=JA F5=48/64 Sh=pas Alt=extra", GEM_DGRAY, GEM_WHITE);

    int max_y = gy + gh - 24;
    /* gutters for left/right more-text arrows */
    nt_view_left  = gx + 10;
    nt_view_right = gx + gw - 10;
    layout_note(scroll, max_y);

    bool more_left  = (nt_hscroll > 0);
    bool more_right = (nt_content_max_x - nt_hscroll > (nt_view_right - nt_view_left));

    /* heading bars — only the visible band */
    {
        int last_hy = -999;
        for (int i = 0; i < nt_ncells; i++) {
            nt_cell_t *c = &nt_cells[i];
            if (!(c->style & STYLE_H1) || c->y == last_hy) continue;
            if (c->y >= max_y || c->y < gy) continue;
            int hh = c->lh ? c->lh : line_height_for(c->style);
            if (hh < 1) hh = 1;
            int bar_h = hh - 1;
            if (c->y + bar_h > max_y) bar_h = max_y - c->y;
            if (bar_h > 0)
                gfx_fill_rect(nt_view_left, c->y, nt_view_right - nt_view_left, bar_h, GEM_GREEN);
            last_hy = c->y;
        }
    }

    /* Pass 1: bases/Latin. Pass 2: sandhangan overlays. */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < nt_ncells; i++) {
            nt_cell_t *c = &nt_cells[i];
            if (c->y >= max_y) continue; /* don't assume y-sorted when lh bumps */
            if (c->y + (c->lh ? c->lh : LATIN_H) < gy) continue;
            if (c->cp == 0 || c->cp == '\n') continue;
            bool is_mark = font_jawa_is_mark(c->cp);
            bool is_pas  = (c->style & STYLE_PASANGAN) != 0;
            /* pass0: bases/latin; pass1: marks + pasangan (stack on top/under) */
            if (pass == 0 && (is_mark || is_pas)) continue;
            if (pass == 1 && !is_mark && !is_pas) continue;

            int sx = c->x - nt_hscroll + nt_view_left;
            int gw_cell = (is_mark || is_pas) ? font_jawa_w() : (c->advance ? c->advance : LATIN_W);
            if (font_jawa_glyph(c->cp) && !is_pas) gw_cell = font_jawa_w();
            if (sx + gw_cell < nt_view_left - 4 || sx > nt_view_right + 4) continue;

            if (c->cp == 0x2022) {
                int mid = c->y + (c->lh ? c->lh : LATIN_H) / 2;
                if (sx >= nt_view_left && sx < nt_view_right)
                    gfx_fill_circle(sx + 5, mid, 3, GEM_BLACK);
                continue;
            }
            uint8_t fg = GEM_BLACK, bg = 0xFF;
            if (c->style & STYLE_H1) fg = GEM_WHITE;
            else if (c->style & STYLE_H2) fg = GEM_GREEN;

            int lh = c->lh ? c->lh : line_height_for(c->style);

            /* Hide pangkon when a pasangan follows at the same origin — the
             * pasangan glyph replaces the virama visually (Unicode still has ꧀). */
            if (c->cp == JAWA_PANGKON) {
                bool has_pas = false;
                for (int j = i + 1; j < nt_ncells; j++) {
                    if (nt_cells[j].cp == '\n') break;
                    if (nt_cells[j].x == c->x && nt_cells[j].y == c->y &&
                        (nt_cells[j].style & STYLE_PASANGAN)) {
                        has_pas = true;
                        break;
                    }
                    if (nt_cells[j].advance) break;
                }
                if (has_pas) continue;
            }

            if (is_pas) {
                draw_jawa_pasangan(sx, c->y, c->cp, fg);
            } else if (is_mark) {
                int mx = sx, my = c->y;
                /* Taling / dirga mure: same stack origin, blit shifted left. */
                if (font_jawa_is_left_mark(c->cp))
                    mx += font_jawa_left_mark_dx();
                /* Below vowels on a pasangan cluster use the baked u.ns.pas
                 * (etc.) form at the same origin — no crude offset of normal
                 * suku. Medials stay on the regular mark glyph. */
                bool use_pas_below = font_jawa_is_below_vowel(c->cp) &&
                                     stack_has_pasangan(c->x, c->y) &&
                                     font_jawa_pas_below(c->cp) != 0;
                /* Skip if entirely outside the horizontal view (use draw x). */
                int jw = font_jawa_w();
                if (mx + jw < nt_view_left - 4 || mx > nt_view_right + 4) continue;
                if (my >= max_y) continue;
                if (use_pas_below)
                    draw_jawa_pas_below(mx, my, c->cp, fg);
                else
                    draw_jawa_glyph(mx, my, c->cp, fg, 0xFF, false);
            } else {
                draw_cp(sx, c->y, c->cp, fg, bg, c->style, lh);
                if ((c->style & STYLE_H2) && c->advance > 0) {
                    int hw = c->advance;
                    if (sx < nt_view_right && sx + hw > nt_view_left) {
                        int hx0 = sx < nt_view_left ? nt_view_left : sx;
                        int hx1 = sx + hw > nt_view_right ? nt_view_right : sx + hw;
                        if (hx1 > hx0) gfx_hline(hx0, c->y + lh - 2, hx1 - hx0, GEM_GREEN);
                    }
                }
            }
        }
    }

    /* cursor */
    if (nt_cur_cell >= 0 && nt_cur_cell < nt_ncells) {
        nt_cell_t *c = &nt_cells[nt_cur_cell];
        int sx = c->x - nt_hscroll + nt_view_left;
        int cw = c->advance ? c->advance : ((c->style & STYLE_JAWA) ? font_jawa_w() / 2 : LATIN_W);
        if (font_jawa_is_mark(c->cp) || (c->style & STYLE_PASANGAN)) cw = font_jawa_w() / 2;
        if (cw < 8) cw = 8;
        int ch = c->lh ? c->lh : line_height_for(c->style);
        int cy = c->y + ch - 2;
        if (cy >= gy && cy + 2 <= max_y &&
            sx + cw > nt_view_left && sx < nt_view_right) {
            int rx = sx < nt_view_left ? nt_view_left : sx;
            int rw = (sx + cw > nt_view_right ? nt_view_right : sx + cw) - rx;
            if (rw > 0) gfx_fill_rect(rx, cy, rw, 2, GEM_GREEN);
        }
    }

    /* more-text arrow indicators */
    int mid_y = gy + (max_y - gy) / 2;
    if (more_left) {
        gfx_fill_rect(gx + 1, mid_y - 8, 8, 16, GEM_LGRAY);
        gfx_puts_at(gx + 1, mid_y - 4, "<", GEM_BLACK, GEM_LGRAY);
    }
    if (more_right) {
        gfx_fill_rect(gx + gw - 9, mid_y - 8, 8, 16, GEM_LGRAY);
        gfx_puts_at(gx + gw - 9, mid_y - 4, ">", GEM_BLACK, GEM_LGRAY);
    }
}

/* ---------- keyboard map overlay ---------- */

static void draw_jawa_cp_at(int x, int y, uint32_t cp) {
    draw_jawa_glyph(x, y, cp, GEM_BLACK, GEM_LGRAY, false);
}

/* Compact key map: multi-page with arrows. Cell size follows active font. */
static void show_keymap(void) {
    typedef struct { uint32_t cp; const char *key; } map_ent_t;
    static const map_ent_t ents[] = {
        /* sandhangan */
        {0xA9B6,"1"},{0xA9B8,"2"},{0xA9BA,"3"},{0xA9B4,"4"},
        {0xA9BC,"5"},{0xA9BF,"6"},{0xA9BE,"7"},{0xA9BD,"8"},
        {0xA981,"9"},{0xA982,"0"},{0xA983,"-"},{0xA9C0,"="},
        {0xA9B7,"A1"},{0xA9B9,"A2"},{0xA9BB,"A3"},{0xA9B5,"A4"},
        {0xA9B3,"S-"},{0xA980,"S="},
        /* carakan */
        {0xA9B2,"Q"},{0xA9A4,"W"},{0xA995,"E"},{0xA9AB,"R"},
        {0xA98F,"T"},{0xA9A2,"Y"},{0xA9A0,"U"},{0xA9B1,"I"},
        {0xA9AE,"O"},{0xA9AD,"P"},{0xA9A5,"A"},{0xA99D,"S"},
        {0xA997,"D"},{0xA9AA,"F"},{0xA99A,"G"},{0xA9A9,"H"},
        {0xA992,"J"},{0xA9A7,"K"},{0xA99B,"L"},{0xA994,";"},
        /* murda / rare (Alt / Alt+Shift) */
        {0xA99F,"Aw"},{0xA996,"Ae"},{0xA9AC,"Ar"},{0xA991,"At"},
        {0xA9A3,"Ay"},{0xA9A1,"Au"},{0xA9AF,"Ai"},{0xA9B0,"ASi"},
        {0xA9A6,"Aa"},{0xA99E,"As"},{0xA999,"Ad"},{0xA998,"Ag"},
        {0xA993,"Aj"},{0xA9A8,"Ak"},{0xA99C,"Al"},{0xA990,"ASt"},
        /* vowels + pada */
        {0xA984,"Z"},{0xA986,"X"},{0xA988,"C"},{0xA98C,"V"},
        {0xA98E,"B"},{0xA985,"Sz"},{0xA987,"Sx"},{0xA989,"Sc"},
        {0xA98D,"Sv"},{0xA98A,"Sb"},{0xA98B,"ASb"},
        {0xA9CA,"N"},{0xA9CB,"M"},{0xA9C8,","},{0xA9C9,"."},
        {0xA9CF,"/"},{0xA9CC,"S/"},{0xA9CD,"A8"},
        {0xA9C1,"Sn"},{0xA9C2,"Sm"},{0xA9C7,"S,"},{0xA9C6,"S."},
        {0xA9C3,"A5"},{0xA9C4,"A6"},{0xA9C5,"A7"},
        {0xA9DE,"AS-"},{0xA9DF,"AS="},
        /* digits */
        {0xA9D0,"S0"},{0xA9D1,"S1"},{0xA9D2,"S2"},{0xA9D3,"S3"},
        {0xA9D4,"S4"},{0xA9D5,"S5"},{0xA9D6,"S6"},{0xA9D7,"S7"},
        {0xA9D8,"S8"},{0xA9D9,"S9"},
    };
    const int nent = (int)(sizeof ents / sizeof ents[0]);
    int jw = font_jawa_w(), jh = font_jawa_h();
    int cell_w = jw + 30, cell_h = jh + 28;
    int cols = (gw - 12) / cell_w;
    if (cols < 2) cols = 2;
    if (cols > 4) cols = 4;
    int rows = (gh - 36) / cell_h;
    if (rows < 1) rows = 1;
    int per_page = cols * rows;
    int page = 0;
    int pages = (nent + per_page - 1) / per_page;

    for (;;) {
        jw = font_jawa_w(); jh = font_jawa_h();
        cell_w = jw + 30; cell_h = jh + 28;
        cols = (gw - 12) / cell_w; if (cols < 2) cols = 2; if (cols > 4) cols = 4;
        rows = (gh - 36) / cell_h; if (rows < 1) rows = 1;
        per_page = cols * rows;
        pages = (nent + per_page - 1) / per_page;
        if (page >= pages) page = pages - 1;

        draw_frame("CARAKAN MAP");
        char hdr[48];
        snprintf(hdr, sizeof hdr, "A=Alt S=Sh  F5=%dpx  p%d/%d",
                 font_jawa_px(), page + 1, pages);
        gfx_puts_fit(gx + 2, gy + 1, hdr, GEM_DGRAY, GEM_WHITE, gw - 4);

        int x0 = gx + 6, y0 = gy + 14;
        int start = page * per_page;
        for (int i = 0; i < per_page && start + i < nent; i++) {
            int col = i % cols, row = i / cols;
            int x = x0 + col * cell_w;
            int y = y0 + row * cell_h;
            gfx_fill_rect(x, y, cell_w - 4, cell_h - 4, GEM_LGRAY);
            gfx_rect(x, y, cell_w - 4, cell_h - 4, GEM_BLACK);
            draw_jawa_cp_at(x + 4, y + 4, ents[start + i].cp);
            const char *lab = ents[start + i].key;
            int lx = x + cell_w - 4 - (int)strlen(lab) * FONT_W * 2;
            if (lx < x + 4) lx = x + 4;
            for (int k = 0; lab[k]; k++)
                gfx_glyph_scale(lx + k * FONT_W * 2, y + cell_h - 20, lab[k], 2,
                                GEM_DGRAY, GEM_LGRAY, false, false);
        }
        gfx_puts_fit(gx + 2, gy + gh - 10, "LEFT/RIGHT=page F5=size ESC=back", GEM_DGRAY, GEM_WHITE, gw - 4);
        gfx_flush();
        int c; wait_key(&c);
        sound_click();
        if (c == KEY_ESC || c == KEY_ENTER) return;
        if (c == KEY_F5) {
            font_jawa_set_px(font_jawa_px() == 64 ? 48 : 64);
            continue;
        }
        if ((c == KEY_RIGHT || c == KEY_DOWN) && page < pages - 1) page++;
        if ((c == KEY_LEFT || c == KEY_UP) && page > 0) page--;
    }
}

/* ---------- file browser ---------- */

static int list_notes(char names[][48], int max) {
    nt_ensure_dir();
    char all[32][48];
    int n = sdfs_list_dir(NT_PATH, all, 32, false);
    int out = 0;
    for (int i = 0; i < n && out < max; i++) {
        int l = (int)strlen(all[i]);
        if (l > 3) {
            const char *ext = all[i] + l - 3;
            if (!strcasecmp(ext, ".md") || !strcasecmp(ext, ".MD")) {
                snprintf(names[out], 48, "%s", all[i]);
                out++;
            }
        }
    }
    return out;
}

static bool load_note(const char *name) {
    char path[64];
    snprintf(path, sizeof path, "%s/%s", NT_PATH, name);
    uint32_t rl = 0;
    if (!sdfs_read_file(path, nt_buf, NT_MAX, &rl)) return false;
    nt_len = (int)rl;
    if (nt_len >= NT_MAX) nt_len = NT_MAX - 1;
    nt_buf[nt_len] = 0;
    nt_cur = 0;
    snprintf(nt_file, sizeof nt_file, "%s", name);
    nt_dirty = false;
    return true;
}

static bool save_note(void) {
    if (!os_sd_present) return false;
    nt_ensure_dir();
    if (!nt_file[0]) {
        /* auto name */
        char names[32][48];
        int n = list_notes(names, 32);
        snprintf(nt_file, sizeof nt_file, "note%02d.md", n + 1);
    }
    char path[64];
    snprintf(path, sizeof path, "%s/%s", NT_PATH, nt_file);
    if (sdfs_write_file(path, nt_buf, (uint32_t)nt_len)) {
        nt_dirty = false;
        return true;
    }
    return false;
}

static void prompt_filename(void) {
    char name[40] = "note.md";
    int len = (int)strlen(name);
    int cur = len - 3; /* before .md */
    for (;;) {
        draw_frame("NEW NOTE");
        gfx_puts_fit(gx + 4, gy + 20, "Filename:", GEM_DGRAY, GEM_WHITE, gw - 8);
        gfx_fill_rect(gx + 4, gy + 36, gw - 8, 16, GEM_LGRAY);
        gfx_rect(gx + 4, gy + 36, gw - 8, 16, GEM_BLACK);
        gfx_puts_fit(gx + 8, gy + 40, name, GEM_BLACK, GEM_LGRAY, gw - 16);
        if (gx + 8 + cur * FONT_W + FONT_W <= gx + gw - 4)
            gfx_fill_rect(gx + 8 + cur * FONT_W, gy + 40 + FONT_H - 1, FONT_W, 1, GEM_GREEN);
        gfx_puts_fit(gx + 4, gy + 60, "ENTER=create  ESC=cancel", GEM_DGRAY, GEM_WHITE, gw - 8);
        gfx_flush();
        int c; wait_key(&c);
        sound_click();
        if (c == KEY_ESC) { nt_file[0] = 0; return; }
        if (c == KEY_ENTER) {
            if (len < 4) continue;
            /* ensure .md */
            if (len < 3 || strcasecmp(name + len - 3, ".md") != 0) {
                if (len < 36) { memcpy(name + len, ".md", 4); len += 3; }
            }
            snprintf(nt_file, sizeof nt_file, "%s", name);
            return;
        }
        if (c == KEY_LEFT && cur > 0) cur--;
        else if (c == KEY_RIGHT && cur < len) cur++;
        else if (c == KEY_BACKSPACE && cur > 0) {
            memmove(name + cur - 1, name + cur, len - cur + 1);
            cur--; len--;
        } else if (c >= 32 && c < 127 && len < 38) {
            char ch = (char)c;
            if (isalnum((unsigned char)ch) || ch == '-' || ch == '_' || ch == '.') {
                memmove(name + cur + 1, name + cur, len - cur + 1);
                name[cur++] = ch;
                len++;
            }
        }
    }
}

/* ---------- editor loop ---------- */

static int cursor_line(void) {
    int line = 0;
    for (int i = 0; i < nt_cur && i < nt_len; i++)
        if (nt_buf[i] == '\n') line++;
    return line;
}

/* Count lines in the note (always >= 1). */
static int note_line_count(void) {
    int lines = 1;
    for (int i = 0; i < nt_len; i++)
        if (nt_buf[i] == '\n') lines++;
    return lines;
}

/* Keep cursor on-screen: vertical via logical-line scroll, horizontal via hscroll. */
static void ensure_cursor_visible(int *scroll, int max_y) {
    int top = gy + 2;
    int view_w = nt_view_right - nt_view_left;
    if (view_w < 40) view_w = gw - 20;
    int view_h = max_y - top;
    if (view_h < 8) view_h = 8;

    int nlines = note_line_count();
    int cl = cursor_line();
    if (cl < 0) cl = 0;
    if (cl >= nlines) cl = nlines - 1;

    /* Never let vertical scroll leave the document. */
    if (*scroll < 0) *scroll = 0;
    if (*scroll >= nlines) *scroll = nlines - 1;
    if (*scroll < 0) *scroll = 0;

    /* If scroll is past the cursor line, pull back so the cursor can appear. */
    if (*scroll > cl) *scroll = cl;

    for (int tries = 0; tries < 48; tries++) {
        layout_note(*scroll, max_y);

        /* Find the cell for the cursor (layout may have truncated). */
        int ci = -1;
        for (int i = 0; i < nt_ncells; i++) {
            if (nt_cells[i].byte_off >= nt_cur) { ci = i; break; }
        }
        if (ci < 0) {
            /* Cursor is above the window — scroll up. */
            if (*scroll > 0) { (*scroll)--; continue; }
            nt_cur_cell = nt_ncells > 0 ? 0 : -1;
            break;
        }
        nt_cur_cell = ci;
        nt_cell_t *c = &nt_cells[ci];
        int lh = c->lh ? c->lh : line_height_for(c->style);
        if (lh < 1) lh = 1;
        int cw = c->advance ? c->advance : LATIN_W;
        if (font_jawa_is_mark(c->cp) || (c->style & STYLE_PASANGAN))
            cw = font_jawa_w() / 2;
        if (font_jawa_glyph(c->cp) && c->advance)
            cw = c->advance;
        if (cw < 8) cw = 8;

        /* Tall Jawa line taller than the view: pin to cursor line and stop. */
        if (lh >= view_h) {
            *scroll = cl;
            layout_note(*scroll, max_y);
            break;
        }

        if (c->y < top) {
            if (*scroll == 0) break;
            (*scroll)--;
            continue;
        }
        if (c->y + lh > max_y) {
            if (*scroll >= nlines - 1) break;
            (*scroll)++;
            continue;
        }

        if (c->x < nt_hscroll) {
            nt_hscroll = c->x - 8;
            if (nt_hscroll < 0) nt_hscroll = 0;
            continue;
        }
        if (c->x + cw > nt_hscroll + view_w) {
            nt_hscroll = c->x + cw - view_w + 8;
            if (nt_hscroll < 0) nt_hscroll = 0;
            continue;
        }
        {
            int max_hs = nt_content_max_x - view_w + 8;
            if (max_hs < 0) max_hs = 0;
            if (nt_hscroll > max_hs) nt_hscroll = max_hs;
        }
        return;
    }

    if (*scroll < 0) *scroll = 0;
    if (*scroll >= nlines) *scroll = nlines - 1;
    if (*scroll < 0) *scroll = 0;
    if (nt_hscroll < 0) nt_hscroll = 0;
}

static void editor_loop(void) {
    int scroll = 0;
    nt_jawa = false;
    nt_raw = false;
    nt_hscroll = 0;

    draw_frame("NOTES");   /* establish gx,gy,gw,gh before first layout */
    nt_view_left  = gx + 10;
    nt_view_right = gx + gw - 10;

    for (;;) {
        int max_y = gy + gh - 24;
        int page_w = nt_view_right - nt_view_left;
        if (page_w < 40) page_w = gw - 20;
        int page_h = max_y - (gy + 2);
        if (page_h < font_jawa_h()) page_h = font_jawa_h();

        ensure_cursor_visible(&scroll, max_y);
        render_note(scroll);
        gfx_flush();

        int c; wait_key(&c);
        sound_click();

        if (c == KEY_ESC) {
            if (nt_dirty) {
                gfx_puts_at(gx + 2, gy + gh - 10, "F2=save  ESC=discard", GEM_GREEN, GEM_WHITE);
                gfx_flush();
                int c2; wait_key(&c2);
                if (c2 == KEY_F2) save_note();
            }
            return;
        }
        if (c == KEY_F1) { show_keymap(); continue; }
        if (c == KEY_F2) { save_note(); continue; }
        if (c == KEY_F3) { nt_jawa = !nt_jawa; continue; }
        if (c == KEY_F4) { nt_raw = !nt_raw; continue; }
        if (c == KEY_F5) {
            font_jawa_set_px(font_jawa_px() == 64 ? 48 : 64);
            continue;
        }

        /* Shift+arrows = page scroll one whole screen */
        if (kbd_shift_held() &&
            (c == KEY_LEFT || c == KEY_RIGHT || c == KEY_UP || c == KEY_DOWN)) {
            int nlines = note_line_count();
            if (c == KEY_LEFT) {
                nt_hscroll -= page_w;
                if (nt_hscroll < 0) nt_hscroll = 0;
            } else if (c == KEY_RIGHT) {
                nt_hscroll += page_w;
                int max_hs = nt_content_max_x - page_w + 8;
                if (max_hs < 0) max_hs = 0;
                if (nt_hscroll > max_hs) nt_hscroll = max_hs;
            } else if (c == KEY_UP) {
                int jump = page_h / (LATIN_H + 2);
                if (jump < 1) jump = 1;
                scroll -= jump;
                if (scroll < 0) scroll = 0;
            } else if (c == KEY_DOWN) {
                int jump = page_h / (LATIN_H + 2);
                if (jump < 1) jump = 1;
                scroll += jump;
                if (scroll >= nlines) scroll = nlines - 1;
                if (scroll < 0) scroll = 0;
            }
            continue;
        }

        if (c == KEY_LEFT) { nt_cur = utf8_prev(nt_buf, nt_cur); continue; }
        if (c == KEY_RIGHT) { nt_cur = utf8_next(nt_buf, nt_len, nt_cur); continue; }
        if (c == KEY_HOME) {
            while (nt_cur > 0 && nt_buf[nt_cur - 1] != '\n') nt_cur--;
            nt_hscroll = 0;
            continue;
        }
        if (c == KEY_END) {
            while (nt_cur < nt_len && nt_buf[nt_cur] != '\n') nt_cur++;
            continue;
        }
        if (c == KEY_UP) {
            int col = 0, p = nt_cur;
            while (p > 0 && nt_buf[p - 1] != '\n') { p = utf8_prev(nt_buf, p); col++; }
            if (p > 0) {
                p--;
                int line_start = p;
                while (line_start > 0 && nt_buf[line_start - 1] != '\n')
                    line_start = utf8_prev(nt_buf, line_start);
                int i = line_start, n = 0;
                while (i < p && n < col) {
                    i = utf8_next(nt_buf, nt_len, i);
                    n++;
                }
                nt_cur = i;
            }
            continue;
        }
        if (c == KEY_DOWN) {
            int col = 0, p = nt_cur;
            while (p > 0 && nt_buf[p - 1] != '\n') { p = utf8_prev(nt_buf, p); col++; }
            while (p < nt_len && nt_buf[p] != '\n') p = utf8_next(nt_buf, nt_len, p);
            if (p < nt_len) {
                p++;
                int i = p, n = 0;
                while (i < nt_len && nt_buf[i] != '\n' && n < col) {
                    i = utf8_next(nt_buf, nt_len, i);
                    n++;
                }
                nt_cur = i;
            }
            continue;
        }
        if (c == KEY_BACKSPACE) { nt_backspace(); continue; }
        if (c == KEY_DEL) { nt_delete(); continue; }
        if (c == KEY_ENTER) { nt_insert_cp('\n'); nt_hscroll = 0; continue; }

        if (kbd_ctrl_held()) {
            if (c == 2 || c == 'b' || c == 'B') { nt_wrap_md("**", "**"); continue; }
            if (c == 9 || c == 'i' || c == 'I') { nt_wrap_md("*", "*"); continue; }
            if (c == 'h' || c == 'H') { nt_insert_bytes("# ", 2); continue; }
            continue;
        }

        if (nt_jawa) {
            /* Layers:
             *   letter        → nglegena (base aksara)
             *   Shift+letter  → pasangan = pangkon + consonant (꧀ꦏ etc.)
             *   Alt+letter    → murda / mahaprana where it exists
             *   Alt+digit     → rare sandhangan / pada
             *   Alt+Shift     → ka sasak, sa mahaprana, nga lelet raswadi, …
             *   digit row     → sandhangan; Shift+digit → Jawa digits
             */
            bool sh = false, alt = kbd_alt_held();
            int key = c;
            if (c >= 'A' && c <= 'Z') {
                key = c - 'A' + 'a';
                sh = true;
            } else if (c >= 'a' && c <= 'z') {
                key = c;
                sh = kbd_shift_held();
            } else {
                static const char *from = "!@#$%^&*()_+<>?:";
                static const char *to   = "1234567890-=,./;";
                const char *p = strchr(from, c);
                if (p) { key = to[(int)(p - from)]; sh = true; }
                else if (kbd_shift_held()) sh = true;
            }

            /* Alt+Shift = rare forms; Alt alone = murda / extra sandhangan */
            if (alt) {
                uint32_t extra = jawa_map_alt(key, sh);
                if (extra) { nt_insert_cp(extra); continue; }
                if (key >= 'a' && key <= 'z') {
                    uint32_t cp = jawa_map_key(key, true);
                    if (cp) nt_insert_cp(cp);
                    continue;
                }
            }

            /* Shift + carakan letter = pasangan (store as ꧀ + aksara) */
            if (sh && key >= 'a' && key <= 'z') {
                uint32_t base = jawa_map_key(key, false);
                if (jawa_can_pasangan(base)) {
                    nt_insert_cp(JAWA_PANGKON);
                    nt_insert_cp(base);
                    continue;
                }
            }

            /* Shift on digit/symbol row, or plain letter */
            uint32_t cp = jawa_map_key(key, sh && !(key >= 'a' && key <= 'z'));
            if (!cp && !sh) cp = jawa_map_key(key, false);
            if (cp) nt_insert_cp(cp);
            continue;
        }

        if (c >= 32 && c < 127) nt_insert_cp((uint32_t)c);
    }
}

/* ---------- main menu ---------- */

void app_notes(void) {
    char names[24][48];
    int sel = 0;

    for (;;) {
        int n = os_sd_present ? list_notes(names, 24) : 0;
        draw_frame("NOTES");
        gfx_puts_fit(gx + 4, gy + 2, "N=new  ENTER=open  F1=help", GEM_DGRAY, GEM_WHITE, gw - 8);

        int total = n + 1;
        if (sel >= total) sel = total - 1;
        if (sel < 0) sel = 0;
        int row_h = FONT_H + 1;
        int vis = (gh - 40) / row_h;
        if (vis < 1) vis = 1;
        int top = sel - vis + 1;
        if (top < 0) top = 0;
        if (sel < top) top = sel;
        int y = gy + 16;
        for (int i = 0; i < vis && top + i < total; i++) {
            int idx = top + i;
            uint8_t fg = (idx == sel) ? GEM_WHITE : GEM_BLACK;
            uint8_t bg = (idx == sel) ? GEM_GREEN : GEM_WHITE;
            gfx_fill_rect(gx + 2, y, gw - 4, FONT_H, bg);
            if (idx == 0) gfx_puts_fit(gx + 6, y, "+ New note", fg, bg, gw - 12);
            else gfx_puts_fit(gx + 6, y, names[idx - 1], fg, bg, gw - 12);
            y += row_h;
        }
        if (top > 0) gfx_puts_at(gx + gw - 10, gy + 16, "^", GEM_DGRAY, GEM_WHITE);
        if (top + vis < total) gfx_puts_at(gx + gw - 10, gy + gh - 26, "v", GEM_DGRAY, GEM_WHITE);
        if (!os_sd_present)
            gfx_puts_fit(gx + 4, gy + gh - 24, "No SD — notes stay in RAM", GEM_DGRAY, GEM_WHITE, gw - 8);
        gfx_puts_fit(gx + 2, gy + gh - 10, "ESC=close", GEM_DGRAY, GEM_WHITE, gw - 4);
        gfx_flush();

        int c; wait_key(&c);
        sound_click();
        if (c == KEY_ESC) return;
        if (c == KEY_F1) { show_keymap(); continue; }
        if (c == KEY_UP && sel > 0) sel--;
        if (c == KEY_DOWN && sel < total - 1) sel++;
        if (c == 'n' || c == 'N') {
            nt_len = 0; nt_cur = 0; nt_buf[0] = 0; nt_dirty = false;
            if (os_sd_present) prompt_filename();
            else snprintf(nt_file, sizeof nt_file, "RAM.md");
            if (nt_file[0] || !os_sd_present) {
                if (!nt_file[0]) snprintf(nt_file, sizeof nt_file, "RAM.md");
                editor_loop();
            }
            sel = 0;
            continue;
        }
        if (c == KEY_ENTER) {
            if (sel == 0) {
                nt_len = 0; nt_cur = 0; nt_buf[0] = 0; nt_dirty = false;
                if (os_sd_present) prompt_filename();
                else snprintf(nt_file, sizeof nt_file, "RAM.md");
                if (nt_file[0] || !os_sd_present) {
                    if (!nt_file[0]) snprintf(nt_file, sizeof nt_file, "RAM.md");
                    editor_loop();
                }
            } else if (n > 0) {
                if (load_note(names[sel - 1])) editor_loop();
            }
        }
    }
}
