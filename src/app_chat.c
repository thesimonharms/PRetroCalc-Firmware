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
 * defaults for the rest. */
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

static int gx, gy, gw, gh;

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

/* tiny line editor inside the window */
static int win_read_line(int y, const char *prompt, char *buf, int maxlen, bool secret) {
    int len = 0; buf[0] = 0;
    for (;;) {
        gfx_fill_rect(gx, y, gw, 10, GEM_WHITE);
        gfx_puts_at(gx, y, prompt, GEM_DGRAY, GEM_WHITE);
        int px = gx + strlen(prompt) * 8;
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

/* naive JSON string extractor: finds "key":"...": returns pointer into static buf */
static const char *json_str(const char *json, const char *key) {
    static char out[1024];
    char pat[40];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return NULL;
    p = strchr(p + strlen(pat), ':');
    if (!p) return NULL;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return NULL;
    p++;
    int i = 0;
    while (*p && *p != '"' && i < (int)sizeof(out) - 1) {
        if (*p == '\\' && p[1]) {
            p++;
            char e = *p;
            out[i++] = (e == 'n') ? '\n' : (e == 't') ? '\t' : e;
            p++;
        } else out[i++] = *p++;
    }
    out[i] = 0;
    return out;
}

/* escape user text for embedding into a JSON string */
static void json_escape(const char *in, char *out, int max) {
    int i = 0;
    for (; *in && i < max - 2; in++) {
        if (*in == '"' || *in == '\\') { out[i++] = '\\'; out[i++] = *in; }
        else if (*in == '\n') { out[i++] = '\\'; out[i++] = 'n'; }
        else out[i++] = *in;
    }
    out[i] = 0;
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
    for (int i = 0; i < n; i++) { /* strip \r / trailing spaces */
        char *e = lines[i] + strlen(lines[i]);
        while (e > lines[i] && (e[-1] == '\r' || e[-1] == ' ')) *--e = 0;
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

void app_chat(void) {
    draw_frame("LLM CHAT");
    gfx_puts_at(gx + 4, gy + 8, "WiFi LLM chat (plain HTTP, LAN servers)", GEM_DGRAY, GEM_WHITE);
    gfx_puts_at(gx + 4, gy + 20, "Ollama / llama.cpp / OpenAI-compatible", GEM_DGRAY, GEM_WHITE);
    gfx_flush();

    bool have_cfg = load_config();
    if (!have_cfg) {
        strncpy(port_s, "11434", 7);
        strncpy(path, "/api/generate", 127);
        strncpy(model, "llama3", 47);
        if (win_read_line(gy + 40, "SSID: ", ssid, sizeof ssid, false) < 0) return;
        if (win_read_line(gy + 54, "PASS: ", pass, sizeof pass, true) < 0) return;
        if (win_read_line(gy + 68, "HOST: ", host, sizeof host, false) < 0) return;
    }

    /* connect */
    gfx_puts_at(gx + 4, gy + 90, "Connecting to WiFi...", GEM_BLACK, GEM_WHITE);
    gfx_flush();
    if (!net_connect(ssid, pass, 15000)) {
        gfx_puts_at(gx + 4, gy + 104, "WiFi connect FAILED. Check SSID/pass.", GEM_GREEN, GEM_WHITE);
        gfx_puts_at(gx, gy + gh - 10, "Any key=exit", GEM_DGRAY, GEM_WHITE);
        gfx_flush();
        int c; wait_key(&c);
        return;
    }
    char ip[24] = "?";
    net_ip_str(ip, sizeof ip);
    char conn[64]; snprintf(conn, sizeof conn, "Connected: %s", ip);
    gfx_puts_at(gx + 4, gy + 104, conn, GEM_GREEN, GEM_WHITE);
    gfx_puts_at(gx + 4, gy + 118, "ENTER=send  ESC=exit", GEM_DGRAY, GEM_WHITE);
    gfx_flush();

    int port = atoi(port_s);
    static char msg[256];
    static char body[512];
    static char resp[4096];
    int chat_y = gy + 132;

    for (;;) {
        if (win_read_line(chat_y, "You: ", msg, sizeof msg, false) < 0) break;

        /* build request */
        char esc[300];
        json_escape(msg, esc, sizeof esc);
        if (strstr(path, "generate") || strstr(path, "completion"))
            snprintf(body, sizeof body, "{\"model\":\"%s\",\"prompt\":\"%s\",\"stream\":false}", model, esc);
        else /* openai chat completions */
            snprintf(body, sizeof body,
                "{\"model\":\"%s\",\"messages\":[{\"role\":\"user\",\"content\":\"%s\"}]}",
                model, esc);

        gfx_fill_rect(gx, chat_y + 12, gw, 10, GEM_WHITE);
        gfx_puts_at(gx, chat_y + 12, "Thinking...", GEM_DGRAY, GEM_WHITE);
        gfx_flush();

        int rl = net_http_post(host, port, path, body, resp, sizeof resp, 20000);
        gfx_fill_rect(gx, chat_y + 12, gw, gh - (chat_y - gy) - 12, GEM_WHITE);
        if (rl < 0) {
            gfx_puts_at(gx, chat_y + 12, "Request failed (host/path/server?)", GEM_GREEN, GEM_WHITE);
        } else {
            const char *txt = json_str(resp, "response");
            if (!txt) txt = json_str(resp, "content");
            if (!txt) txt = json_str(resp, "text");
            if (!txt) txt = resp; /* fallback: dump raw */
            /* word-wrap into window */
            int x = gx, y = chat_y + 12;
            gfx_puts_at(x, y, "AI: ", GEM_GREEN, GEM_WHITE);
            x += 32;
            for (const char *p = txt; *p && y < gy + gh - 20; p++) {
                if (*p == '\n' || x > gx + gw - 12) { x = gx; y += 8; if (*p == '\n') continue; }
                if (*p >= 32 && *p < 127) gfx_glyph(x, y, *p, GEM_BLACK, GEM_WHITE);
                x += 8;
            }
        }
        gfx_flush();
        /* small chime on reply */
        sound_beep(1200, 40);
    }
    net_disconnect();
}
