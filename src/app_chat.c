/* PRetroCalc OS - LLM chat app (plain HTTP over WiFi).
 * Designed for LAN LLM servers that don't need TLS:
 *   - Ollama:            http://<ip>:11434/api/generate
 *   - llama.cpp server:  http://<ip>:8080/completion
 *   - OpenAI-compatible: http://<ip>:8000/v1/chat/completions
 * Config is stored on SD card as CHAT.CFG:
 *   line1 = ssid
 *   line2 = wifi password
 *   line3 = host (ip or hostname)
 *   line4 = port
 *   line5 = path
 *   line6 = model name
 * If CHAT.CFG is missing, the app prompts for ssid/pass/host and uses Ollama
 * defaults for the rest.
 *
 * UI layout (within the os_window client rect gx,gy,gw,gh):
 *   +------------------------+
 *   | * model name           |  header strip: status dot + label
 *   +------------------------+
 *   | transcript             |  scrolls both ways with Up/Dn/PgUp/PgDn/Home/End
 *   | (persistent + pager)   |
 *   +------------------------+
 *   | You: _                 |  fixed input row (also shows "Thinking...")
 *   +------------------------+
 *
 * Transcript is kept in an in-RAM ring buffer (SB_ROWS lines, up to SB_COLS
 * chars each, with per-cell colour) and rendered through a view_top offset.
 * "Follow" mode keeps the newest line at the bottom of the viewport; scrolling
 * up suspends follow, and landing back on the bottom (End / scrolling down)
 * re-locks it. */
#include "apps.h"
#include "os.h"
#include "gfx.h"
#include "keyboard.h"
#include "sound.h"
#include "net.h"
#include "sdfs.h"
#include "board.h"
#include "pico/stdlib.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

static int gx, gy, gw, gh;       /* client rect from os_window */

#define CHAT_HEADER_H 16         /* status strip at top of client area */
#define CHAT_INPUT_H  14         /* reserved for bottom input row */

#define SB_COLS 40               /* max columns per line (LCD is 320/8=40) */
#define SB_ROWS 256              /* scrollback capacity in lines */

/* transcript region geometry (set up in app_chat) */
static int tx_left, tx_right;
static int ty0, cbot, ty1;       /* top / content bottom / status row bottom */
static int chat_cols;            /* columns that fit in the window */

/* scrollback buffer (ring) */
static char    sb_char[SB_ROWS][SB_COLS];
static uint8_t sb_fg[SB_ROWS][SB_COLS];
static bool    sb_thinking[SB_ROWS]; /* this line is part of a "thinking" block */
static int     sb_len[SB_ROWS];      /* valid chars in this line */
static int     sb_head = 0;          /* ring: index of oldest valid line */
static int     sb_count = 0;        /* number of valid lines (<= SB_ROWS) */
static int     sb_col = 0;           /* write column in the current line */
static bool    sb_writing_thinking = false;  /* lines opened while true get flagged */
static int     view_top = 0;        /* display-row index shown at viewport top */
static bool    sb_follow = true;    /* 1 = view_top tracks newest line */
static bool    thinking_collapsed = true;   /* hide reasoning blocks behind a placeholder */

/* display map: each visible ("display") row maps to one or more buffer lines.
 * When thinking is collapsed, a run of N thinking lines compresses to one
 * placeholder row. view_top is in *display* rows so PgUp/PgDn stay sensible. */
static int disp_to_abs[SB_ROWS + 1];  /* abs line shown at display row r */
static int disp_count = 0;            /* total display rows */

static inline int sb_idx(int abs) { return (sb_head + abs) % SB_ROWS; }
static inline int sb_cur_idx(void) { return (sb_head + sb_count - 1) % SB_ROWS; }

static void sb_open_new(void) {
    if (sb_count >= SB_ROWS) { sb_head = (sb_head + 1) % SB_ROWS; sb_count--; }
    sb_count++;
    int idx = sb_cur_idx();
    sb_len[idx] = 0;
    sb_thinking[idx] = sb_writing_thinking;  /* inherit current writing mode */
    sb_col = 0;
}

static void sb_putc(char c, uint8_t fg) {
    if (c == '\n') { sb_open_new(); return; }
    if (sb_col >= chat_cols) sb_open_new();
    if (c < 32 || c >= 127) return;   /* drop control chars; printspace only */
    int idx = sb_cur_idx();
    sb_char[idx][sb_col] = c;
    sb_fg[idx][sb_col] = fg;
    sb_len[idx] = sb_col + 1;
    sb_col++;
}

static void sb_puts(const char *s, uint8_t fg) {
    for (; *s; s++) sb_putc(*s, fg);
}

/* write a string as part of a "thinking" block: every line opened (including
 * those split by wrap) gets the sb_thinking flag and a dim colour, so the
 * block can be collapsed/expanded as a unit and renders grey when expanded. */
static void sb_puts_thinking(const char *s) {
    bool prev = sb_writing_thinking;
    sb_writing_thinking = true;
    for (; *s; s++) sb_putc(*s, GEM_DGRAY);
    sb_writing_thinking = prev;
}

static int chat_visible_rows(void) { return (cbot - ty0) / 8; }

/* rebuild the display-row -> abs-line map. When thinking is collapsed, a run
 * of consecutive thinking lines compresses into a single placeholder row.
 * Returns the maximum valid view_top (in display rows). */
static int chat_relayout(void) {
    int vrows = chat_visible_rows();
    if (vrows < 1) vrows = 1;
    disp_count = 0;
    int i = 0;
    while (i < sb_count) {
        int idx = sb_idx(i);
        if (sb_thinking[idx] && thinking_collapsed) {
            /* collapse the whole run to one display row (pointing at its start) */
            disp_to_abs[disp_count++] = i;
            while (i < sb_count && sb_thinking[sb_idx(i)]) i++;
        } else {
            disp_to_abs[disp_count++] = i;
            i++;
        }
    }
    int max_top = disp_count - vrows;
    if (max_top < 0) max_top = 0;
    return max_top;
}

static void chat_repaint(void) {
    int vrows = chat_visible_rows();
    if (vrows < 1) vrows = 1;
    int max_top = chat_relayout();
    if (sb_follow) view_top = max_top;
    if (view_top > max_top) view_top = max_top;
    if (view_top < 0) view_top = 0;

    gfx_fill_rect(tx_left, ty0, tx_right - tx_left, cbot - ty0, GEM_WHITE);
    for (int r = 0; r < vrows; r++) {
        int disp = view_top + r;
        if (disp >= disp_count) break;
        int abs = disp_to_abs[disp];
        int idx = sb_idx(abs);
        int y = ty0 + r * 8;

        if (sb_thinking[idx] && thinking_collapsed) {
            /* placeholder for this run: count its length */
            int run = 0;
            while (abs + run < sb_count && sb_thinking[sb_idx(abs + run)]) run++;
            char ph[40];
            snprintf(ph, sizeof ph, "[+ thinking: %d lines - Tab]", run);
            gfx_puts_at(tx_left, y, ph, GEM_DGRAY, GEM_WHITE);
        } else {
            for (int c = 0; c < sb_len[idx]; c++)
                gfx_glyph(tx_left + c * 8, y, sb_char[idx][c], sb_fg[idx][c], GEM_WHITE);
        }
    }

    /* status row: scroll indicators + thinking-state hint */
    gfx_fill_rect(tx_left, cbot, tx_right - tx_left, 8, GEM_WHITE);
    int above = view_top;
    int below = disp_count - (view_top + vrows);
    if (above < 0) above = 0;
    if (below < 0) below = 0;
    char hint[48];
    if (above > 0 || below > 0)
        snprintf(hint, sizeof hint, "^%d v%d  End=live  Tab=%s",
                 above, below, thinking_collapsed ? "show-think" : "hide-think");
    else
        snprintf(hint, sizeof hint, "Tab=%s", thinking_collapsed ? "show-think" : "hide-think");
    gfx_puts_at(tx_left, cbot, hint, GEM_DGRAY, GEM_WHITE);
}

static void chat_scroll(int delta) {
    int max_top = chat_relayout();
    int new_top = view_top + delta;
    if (new_top < 0) new_top = 0;
    if (new_top > max_top) new_top = max_top;
    view_top = new_top;
    sb_follow = (view_top == max_top);
    chat_repaint();
}

static void chat_goto_bottom(void) { sb_follow = true; chat_repaint(); }
static void chat_goto_top(void)    { view_top = 0; sb_follow = (disp_count <= chat_visible_rows()); chat_repaint(); }
static void chat_toggle_thinking(void) { thinking_collapsed = !thinking_collapsed; chat_repaint(); }

static void draw_frame(const char *title) {
    os_gem_desktop_bg();
    os_window(title, &gx, &gy, &gw, &gh);
}

static bool wait_key(int *code) {
    kbd_event_t ev;
    for (;;) {
        kbd_poll(); sound_update(); net_poll();
        if (kbd_get_event(&ev) && ev.type == KBD_EV_PRESS) { *code = ev.code; return true; }
        sleep_ms(4);
    }
}

/* setup-only line editor (no scroll keys) */
static int win_read_line(int y, const char *prompt, char *buf, int maxlen, bool secret) {
    int len = 0; buf[0] = 0;
    for (;;) {
        gfx_fill_rect(gx, y, gw, 10, GEM_WHITE);
        gfx_puts_at(gx + 2, y, prompt, GEM_DGRAY, GEM_WHITE);
        int px = gx + 2 + strlen(prompt) * 8;
        for (int i = 0; i < len && px < gx + gw - 8; i++, px += 8)
            gfx_glyph(px, y, secret ? '*' : buf[i], GEM_BLACK, GEM_WHITE);
        gfx_fill_rect(px, y + 8, 8, 1, GEM_GREEN);
        gfx_flush();
        int c; wait_key(&c);
        sound_click();
        if (c == KEY_ENTER) return len;
        if (c == KEY_ESC) return -1;
        if (c == KEY_BACKSPACE) { if (len > 0) buf[--len] = 0; }
        else if (c >= 32 && c < 127 && len < maxlen - 1) { buf[len++] = c; buf[len] = 0; }
    }
}

static void chat_pick_model(void);   /* forward decl (defined after app_chat) */

/* in-chat input row: line editor with horizontal auto-scroll (the view follows
 * the cursor so long prompts stay visible), plus scroll-review keys and a
 * Tab toggle for thinking-block collapse. Returns len on Enter, -1 on ESC. */
static int chat_input_line(int y, const char *prompt, char *buf, int maxlen) {
    int len = 0; buf[0] = 0;
    int vrows = chat_visible_rows();
    int plen = strlen(prompt);
    /* reserve: prompt (plen) + 1 marker column ("<"/">" indicators) + 1
     * trailing column so the cursor underline never clips off the right edge */
    int avail = chat_cols - plen - 2;
    if (avail < 1) avail = 1;
    int cur = 0;        /* insertion cursor: buf[cur] is the cell to the right of the cursor */
    int in_left = 0;    /* first char of buf shown (horizontal viewport) */
    for (;;) {
        gfx_fill_rect(gx, y, gw, 10, GEM_WHITE);
        /* prompt is always rendered intact at the left */
        gfx_puts_at(gx + 2, y, prompt, GEM_DGRAY, GEM_WHITE);
        int marker_x = gx + 2 + plen * 8;
        /* left scroll indicator in the dedicated marker column */
        if (in_left > 0) gfx_glyph(marker_x, y, '<', GEM_DGRAY, GEM_WHITE);
        int px = marker_x + 8;          /* text starts after the marker column */
        for (int i = in_left; i < len && (i - in_left) < avail; i++, px += 8)
            gfx_glyph(px, y, buf[i], GEM_BLACK, GEM_WHITE);
        /* right scroll indicator at the last visible column when truncated */
        if (len - in_left > avail)
            gfx_glyph(gx + 2 + (chat_cols - 1) * 8, y, '>', GEM_DGRAY, GEM_WHITE);
        int cursor_col = cur - in_left;
        int cx = marker_x + 8 + cursor_col * 8;
        gfx_fill_rect(cx, y + 8, 8, 1, GEM_GREEN);
        gfx_flush();
        int c; wait_key(&c);
        sound_click();
        if (c == KEY_UP)         { chat_scroll(-1);       continue; }
        if (c == KEY_DOWN)       { chat_scroll(1);         continue; }
        if (c == KEY_PAGE_UP)     { chat_scroll(-vrows);    continue; }
        if (c == KEY_PAGE_DOWN)  { chat_scroll(vrows);     continue; }
        if (c == KEY_TAB)        { chat_toggle_thinking(); continue; }
        /* Ctrl+M: the keyboard driver transforms Ctrl+letter to c-'a'+1 in
         * emit(), so by the time we see the event it's 0x0D, not 'm'. */
        if (c == ('m' - 'a' + 1)) { chat_pick_model(); continue; }
        if (c == KEY_LEFT)  { if (cur > 0) cur--;    }
        else if (c == KEY_RIGHT) { if (cur < len) cur++;  }
        else if (c == KEY_HOME)  { cur = 0;                 }
        else if (c == KEY_END)   { cur = len;               }
        else if (c == KEY_ENTER) return len;
        else if (c == KEY_ESC) return -1;
        else if (c == KEY_BACKSPACE) {
            if (cur > 0) { memmove(buf + cur - 1, buf + cur, len - cur + 1); cur--; len--; }
        }
        else if (c >= 32 && c < 127 && len < maxlen - 1) {
            memmove(buf + cur + 1, buf + cur, len - cur + 1);   /* shift tail right */
            buf[cur++] = c; len++;
        }
        /* slide the horizontal viewport so the cursor stays visible */
        if (cur < in_left) in_left = cur;
        if (cur - in_left > avail - 1) in_left = cur - (avail - 1);
        if (in_left < 0) in_left = 0;
    }
}

/* naive JSON string extractor: finds "key":"..." where the key appears as a
 * real key (preceded by { or ,). Avoids matching the word inside string
 * values (e.g. a reasoning model's "thinking" field mentioning "response"). */
static const char *json_str(const char *json, const char *key) {
    static char out[4096];
    char pat[48];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = json;
    const char *found = NULL;
    while ((p = strstr(p, pat)) != NULL) {
        const char *q = p;
        while (q > json && (q[-1] == ' ' || q[-1] == '\n' || q[-1] == '\t' || q[-1] == '\r')) q--;
        if (q > json && (q[-1] == '{' || q[-1] == ',')) { found = p; break; }
        p += strlen(pat);
    }
    if (!found) return NULL;
    p = strchr(found + strlen(pat), ':');
    if (!p) return NULL;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p != '"') return NULL;
    p++;
    int i = 0;
    while (*p && *p != '"' && i < (int)sizeof(out) - 1) {
        if (*p == '\\' && p[1]) {
            p++;
            char e = *p;
            if (e == 'n') { out[i++] = '\n'; p++; }
            else if (e == 't') { out[i++] = '\t'; p++; }
            else if (e == 'u' && p[1] && p[2] && p[3] && p[4]) {
                /* decode \uXXXX to the corresponding byte (ASCII range only;
                 * Ollama escapes < and > as \u003c / \u003e in JSON strings) */
                char hex[5] = { p[1], p[2], p[3], p[4], 0 };
                unsigned int code = (unsigned int)strtoul(hex, NULL, 16);
                out[i++] = (code < 0x80) ? (char)code : '?';
                p += 5;
            }
            else { out[i++] = e; p++; }
        } else out[i++] = *p++;
    }
    out[i] = 0;
    return out;
}

static void json_escape(const char *in, char *out, int max) {
    int i = 0;
    for (; *in && i < max - 2; in++) {
        if (*in == '"' || *in == '\\') { out[i++] = '\\'; out[i++] = *in; }
        else if (*in == '\n') { out[i++] = '\\'; out[i++] = 'n'; }
        else out[i++] = *in;
    }
    out[i] = 0;
}

/* split a model response into its reasoning ("thinking") and answer parts.
 * Some Ollama setups collapse the chain-of-thought into the same `response`
 * field, delimited by `<think>`...`</think>` (sometimes the opening tag is
 * absent -- the model just emits reasoning text and ends it with `</think>`).
 * Some servers instead expose it via a separate "thinking"/"reasoning" JSON
 * field; the caller handles that via json_str() first.
 *
 * `txt` must be writable; on return *think_out / *answer_out point into it
 * (NUL-terminated in place). think_out is NULL when there is no reasoning. */
static void split_thinking(char *txt, char **think_out, char **answer_out) {
    char *close = strstr(txt, "</think>");
    if (!close) { *think_out = NULL; *answer_out = txt; return; }
    char *open = strstr(txt, "<think>");
    char *think_start, *answer_start;
    if (open && open < close) think_start = open + 7;
    else                      think_start = txt;   /* bare prefix before </think> */
    /* trim leading whitespace/newlines from the reasoning */
    while (think_start < close && (*think_start == '\n' || *think_start == ' ' ||
                                   *think_start == '\t' || *think_start == '\r')) think_start++;
    *close = 0;                            /* terminate the reasoning string */
    *think_out = think_start;
    answer_start = close + 8;              /* skip "</think>" */
    while (*answer_start == '\n' || *answer_start == ' ' || *answer_start == '\t' || *answer_start == '\r') answer_start++;
    *answer_out = answer_start;
}

static const char *short_model_name(const char *m) {
    const char *s = strrchr(m, '/');
    s = s ? s + 1 : m;
    static char buf[48];
    strncpy(buf, s, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    char *c = strchr(buf, ':');
    if (c) *c = 0;
    return buf;
}

static char ssid[64], pass[64], host[64], port_s[8], path[128], model[48];

static bool load_config(void) {
    static char cfg[512];
    uint32_t len = 0;
    if (!sdfs_read_file("CHAT.CFG", cfg, sizeof cfg, &len)) return false;
    char *lines[6] = {0};
    int n = 0;
    char *p = cfg;
    lines[n++] = p;
    for (; *p && n < 6; p++) {
        if (*p == '\n') { *p = 0; if (p[1]) lines[n++] = p + 1; }
    }
    for (int i = 0; i < n; i++) {
        char *e = lines[i] + strlen(lines[i]);
        while (e > lines[i] && ((uint8_t)e[-1] < 32 || e[-1] == ' ')) *--e = 0;
    }
    if (n >= 3) {
        strncpy(ssid, lines[0], 63);
        strncpy(pass, lines[1], 63);
        strncpy(host, lines[2], 63);
        strncpy(port_s, n > 3 ? lines[3] : "11434", 7);
        strncpy(path, n > 4 ? lines[4] : "/api/generate", 127);
        strncpy(model, n > 5 ? lines[5] : "llama3", 47);
        return true;
    }
    return false;
}

static void draw_header(uint8_t dot_color, const char *label) {
    gfx_fill_rect(gx, gy, gw, CHAT_HEADER_H, GEM_WHITE);
    gfx_fill_circle(gx + 6, gy + CHAT_HEADER_H / 2, 3, dot_color);
    int max_chars = (gw - 14 - 2) / 8;
    char buf[48];
    int n = (int)strlen(label);
    if (n > max_chars) n = max_chars;
    memcpy(buf, label, n);
    buf[n] = 0;
    gfx_puts_at(gx + 14, gy + 4, buf, GEM_BLACK, GEM_WHITE);
    gfx_hline(gx, gy + CHAT_HEADER_H, gw, GEM_BLACK);
}

/* model picker: queries Ollama's /api/tags, shows a scrollable list, and lets
 * the user pick one with Up/Dn + Enter. On success, `model` is updated and the
 * header is redrawn. Esc cancels. Uses a temporary buffer for the HTTP GET. */
static void chat_pick_model(void) {
    int port = atoi(port_s);
    static char tags_resp[8192];

    gfx_fill_rect(gx, gy + CHAT_HEADER_H + 1, gw, gh - CHAT_HEADER_H - 1, GEM_WHITE);
    gfx_puts_at(gx + 4, gy + CHAT_HEADER_H + 4, "Querying models...", GEM_DGRAY, GEM_WHITE);
    gfx_flush();

    int rl = net_http_get(host, port, "/api/tags", tags_resp, sizeof tags_resp, 15000);
    if (rl < 0) {
        char eb[80];
        snprintf(eb, sizeof eb, "Err %d fetching models", rl);
        gfx_fill_rect(gx, gy + CHAT_HEADER_H + 1, gw, gh - CHAT_HEADER_H - 1, GEM_WHITE);
        gfx_puts_at(gx + 4, gy + CHAT_HEADER_H + 4, eb, COL_RED, GEM_WHITE);
        gfx_puts_at(gx + 4, gy + CHAT_HEADER_H + 16, "Any key=cancel", GEM_DGRAY, GEM_WHITE);
        gfx_flush();
        int c; wait_key(&c);
        chat_repaint();
        return;
    }

    /* extract all "name":"..." values where the " is preceded by { (model entry) */
    static char names[32][64];
    int n_models = 0;
    const char *p = tags_resp;
    for (;;) {
        p = strstr(p, "\"name\"");
        if (!p) break;
        const char *q = p;
        while (q > tags_resp && (q[-1] == ' ' || q[-1] == '\n' || q[-1] == '\t' || q[-1] == '\r')) q--;
        if (q > tags_resp && q[-1] == '{' && n_models < 32) {
            const char *colon = strchr(p + 6, ':');
            if (colon) {
                const char *s = colon + 1;
                while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
                if (*s == '"') {
                    s++;
                    int i = 0;
                    while (*s && *s != '"' && i < 63) {
                        if (*s == '\\' && s[1]) { s++; names[n_models][i++] = *s++; }
                        else names[n_models][i++] = *s++;
                    }
                    names[n_models][i] = 0;
                    n_models++;
                }
            }
        }
        p += 6;
    }

    if (n_models == 0) {
        gfx_fill_rect(gx, gy + CHAT_HEADER_H + 1, gw, gh - CHAT_HEADER_H - 1, GEM_WHITE);
        gfx_puts_at(gx + 4, gy + CHAT_HEADER_H + 4, "No models found on server", COL_RED, GEM_WHITE);
        gfx_puts_at(gx + 4, gy + CHAT_HEADER_H + 16, "Any key=cancel", GEM_DGRAY, GEM_WHITE);
        gfx_flush();
        int c; wait_key(&c);
        chat_repaint();
        return;
    }

    /* selection UI */
    int sel = 0;
    int vrows = chat_visible_rows();
    int top = 0;
    for (;;) {
        gfx_fill_rect(gx, gy + CHAT_HEADER_H + 1, gw, gh - CHAT_HEADER_H - 1, GEM_WHITE);
        gfx_puts_at(gx + 4, gy + CHAT_HEADER_H + 4, "Select model (Up/Dn=Move Enter=OK Esc=Cancel)",
                     GEM_DGRAY, GEM_WHITE);
        int list_y = gy + CHAT_HEADER_H + 16;
        int rows_avail = (gy + gh - 10 - list_y) / 8;
        if (rows_avail < 1) rows_avail = 1;
        if (sel < top) top = sel;
        if (sel >= top + rows_avail) top = sel - rows_avail + 1;
        for (int r = 0; r < rows_avail && top + r < n_models; r++) {
            int y = list_y + r * 8;
            int is_sel = (top + r == sel);
            if (is_sel) gfx_fill_rect(gx + 2, y, gw - 4, 8, COL_BLUE);
            /* truncate name to fit */
            int maxc = (gw - 6) / 8;
            char nm[64];
            int ni = (int)strlen(names[top + r]);
            if (ni > maxc) ni = maxc;
            memcpy(nm, names[top + r], ni);
            nm[ni] = 0;
            gfx_puts_at(gx + 4, y, nm, is_sel ? COL_WHITE : GEM_BLACK, is_sel ? COL_BLUE : GEM_WHITE);
        }
        gfx_flush();
        int c; wait_key(&c);
        sound_click();
        if (c == KEY_UP)         { if (sel > 0) sel--; }
        else if (c == KEY_DOWN)   { if (sel < n_models - 1) sel++; }
        else if (c == KEY_PAGE_UP) { sel -= vrows; if (sel < 0) sel = 0; }
        else if (c == KEY_PAGE_DOWN) { sel += vrows; if (sel > n_models - 1) sel = n_models - 1; }
        else if (c == KEY_ENTER)  {
            strncpy(model, names[sel], sizeof model - 1);
            model[sizeof model - 1] = 0;
            draw_header(COL_GREEN, short_model_name(model));
            sb_puts("* Model switched to: ", GEM_DGRAY);
            sb_puts(short_model_name(model), COL_LGREEN);
            sb_putc('\n', GEM_DGRAY);
            chat_repaint();
            return;
        }
        else if (c == KEY_ESC) { chat_repaint(); return; }
    }
}

void app_chat(void) {
    draw_frame("LLM CHAT");

    /* reset scrollback buffer state for a fresh session */
    sb_head = sb_count = sb_col = view_top = 0;
    sb_follow = true;
    sb_writing_thinking = false;
    thinking_collapsed = true;
    memset(sb_thinking, 0, sizeof sb_thinking);

    bool have_cfg = load_config();
    if (!have_cfg) {
        strncpy(port_s, "11434", 7);
        strncpy(path, "/api/generate", 127);
        strncpy(model, "llama3", 47);
        gfx_puts_at(gx + 4, gy + 8, "No CHAT.CFG found. Enter setup:", GEM_DGRAY, GEM_WHITE);
        gfx_puts_at(gx + 4, gy + 20, "(stored as CHAT.CFG on SD for next time)", GEM_DGRAY, GEM_WHITE);
        gfx_flush();
        if (win_read_line(gy + 44, "SSID: ", ssid, sizeof ssid, false) < 0) return;
        if (win_read_line(gy + 58, "PASS: ", pass, sizeof pass, true) < 0) return;
        if (win_read_line(gy + 72, "HOST: ", host, sizeof host, false) < 0) return;
    }

    /* transcript / input geometry within the client rect */
    tx_left  = gx;
    tx_right = gx + gw;
    chat_cols = gw / 8; if (chat_cols > SB_COLS) chat_cols = SB_COLS;
    ty0 = gy + CHAT_HEADER_H + 1;
    int input_y = gy + gh - CHAT_INPUT_H + 2;
    ty1 = input_y - 3;
    cbot = ty1 - 8;             /* reserve 8px status row for scroll hint */
    /* open the first scrollback line so writes have a target */
    sb_open_new();

    draw_header(COL_AMBER, short_model_name(model));
    gfx_hline(gx, input_y - 2, gw, GEM_BLACK);
    gfx_fill_rect(gx, input_y - 1, gw, CHAT_INPUT_H, GEM_WHITE);
    gfx_flush();

    sb_puts("* Connecting to WiFi...", GEM_DGRAY);
    chat_repaint();
    gfx_flush();

    if (!net_connect(ssid, pass, 15000)) {
        draw_header(COL_RED, short_model_name(model));
        sb_putc('\n', GEM_BLACK);
        sb_puts("! WiFi connect FAILED. Check SSID/pass.", COL_RED);
        sb_putc('\n', GEM_BLACK);
        chat_repaint();
        gfx_puts_at(gx, gy + gh - 10, "Any key=exit", GEM_DGRAY, GEM_WHITE);
        gfx_flush();
        int c; wait_key(&c);
        return;
    }

    draw_header(COL_GREEN, short_model_name(model));
    char ip[24] = "?";
    net_ip_str(ip, sizeof ip);
    char sys[96];
    snprintf(sys, sizeof sys, "* Connected (%s). Server %s:%s  Model %s",
             ip, host, port_s, short_model_name(model));
    sb_puts(sys, GEM_DGRAY);
    sb_putc('\n', GEM_BLACK);
    sb_puts("* Up/Dn scroll, PgUp/PgDn page, Home/End jump, Tab=think, Ctrl+M=model", GEM_DGRAY);
    sb_putc('\n', GEM_BLACK);
    chat_repaint();
    gfx_flush();

    int port = atoi(port_s);
    static char msg[256];
    static char body[512];
    static char resp[12288];

    for (;;) {
        if (chat_input_line(input_y, "You: ", msg, sizeof msg) < 0) break;

        /* echo the user's turn into the transcript */
        sb_puts("You: ", COL_LGREEN);
        sb_puts(msg, GEM_BLACK);
        sb_putc('\n', GEM_BLACK);

        char esc[300];
        json_escape(msg, esc, sizeof esc);
        if (strstr(path, "generate") || strstr(path, "completion"))
            snprintf(body, sizeof body, "{\"model\":\"%s\",\"prompt\":\"%s\",\"stream\":false}", model, esc);
        else /* openai chat completions */
            snprintf(body, sizeof body,
                "{\"model\":\"%s\",\"messages\":[{\"role\":\"user\",\"content\":\"%s\"}]}",
                model, esc);

        /* input row shows "Thinking..." while we wait */
        gfx_fill_rect(gx, input_y - 1, gw, CHAT_INPUT_H, GEM_WHITE);
        gfx_puts_at(gx + 2, input_y, "Thinking...", GEM_DGRAY, GEM_WHITE);
        gfx_flush();

        int rl = net_http_post(host, port, path, body, resp, sizeof resp, 120000);

        gfx_fill_rect(gx, input_y - 1, gw, CHAT_INPUT_H, GEM_WHITE);

        if (rl < 0) {
            const char *why =
                rl == -1 ? "not connected (WiFi dropped)" :
                rl == -2 ? "request too big" :
                rl == -3 ? "DNS resolve failed" :
                rl == -4 ? "out of TCP pcbs" :
                rl == -5 ? "TCP connect refused/unreachable" :
                           "I/O error / empty reply";
            char eb[96];
            snprintf(eb, sizeof eb, "Err %d: %s (recv %d, lwip %d)",
                     rl, why, net_last_recv_len, net_last_err);
            sb_puts("AI: ", COL_GREEN);
            sb_puts(eb, COL_RED);
            sb_putc('\n', GEM_BLACK);
        } else {
            /* reasoning models put their chain-of-thought either in a separate
             * JSON field ("thinking"/"reasoning"/"reasoning_content") or inline
             * in the response text wrapped in <think>...</think> (some Ollama
             * setups emit only the closing tag). Extract it and render it as a
             * collapsible, dimmed block so it doesn't dominate the screen.
             *
             * json_str() uses one shared static buffer, so each extracted field
             * must be copied out before the next json_str() call. */
            static char field_think[4096];
            field_think[0] = 0;
            const char *t = json_str(resp, "thinking");
            if (!t) t = json_str(resp, "reasoning");
            if (!t) t = json_str(resp, "reasoning_content");
            if (t) { strncpy(field_think, t, sizeof field_think - 1); field_think[sizeof field_think - 1] = 0; }

            static char field_resp[12288];
            const char *r = json_str(resp, "response");
            if (!r) r = json_str(resp, "content");
            if (!r) r = json_str(resp, "text");
            const char *src = r ? r : resp;   /* fallback: dump raw */
            strncpy(field_resp, src, sizeof field_resp - 1);
            field_resp[sizeof field_resp - 1] = 0;

            const char *think;
            const char *answer;
            if (field_think[0]) {
                think = field_think;
                answer = field_resp;
            } else {
                char *inline_think = NULL, *ans = NULL;
                split_thinking(field_resp, &inline_think, &ans);
                think = inline_think;      /* NULL when no <think> markers */
                answer = ans ? ans : field_resp;
            }

            if (think && think[0]) {
                sb_puts_thinking(think);
                sb_putc('\n', GEM_DGRAY);   /* terminate the thinking run */
                sb_putc('\n', GEM_BLACK);   /* blank separator */
            }
            sb_puts("AI: ", COL_GREEN);
            for (const char *p = answer; *p; p++) sb_putc(*p, GEM_BLACK);
            sb_putc('\n', GEM_BLACK);
        }
        sb_putc('\n', GEM_BLACK);   /* blank line between turns */
        chat_repaint();
        gfx_flush();
        sound_beep(1200, 40);
    }
    net_disconnect();
}