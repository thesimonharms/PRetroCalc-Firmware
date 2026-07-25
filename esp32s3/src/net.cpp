#include <Arduino.h>
#include <WiFi.h>
#include <string.h>
#include <stdio.h>

extern "C" {
#include "net.h"

int net_last_recv_len = 0;
int net_last_err = 0;
}

static bool inited;
enum { NET_ERR_NOTCONN=-1, NET_ERR_BODY=-2, NET_ERR_DNS=-3, NET_ERR_PCB=-4,
       NET_ERR_CONNECT=-5, NET_ERR_IO=-6 };

static int http_xact(const char *host, uint16_t port, const char *request, char *resp, int resp_max, int timeout_ms) {
    net_last_recv_len = 0; net_last_err = 0;
    if (!host || !request || !resp || resp_max < 2) return NET_ERR_BODY;
    if (!net_is_connected()) return NET_ERR_NOTCONN;
    WiFiClient client; client.setTimeout(timeout_ms);
    if (!client.connect(host, port)) { net_last_err = NET_ERR_CONNECT; return NET_ERR_CONNECT; }
    if (client.print(request) != (int)strlen(request)) { client.stop(); net_last_err = NET_ERR_IO; return NET_ERR_IO; }
    int n = 0; uint32_t last = millis();
    while ((uint32_t)(millis() - last) < (uint32_t)timeout_ms) {
        while (client.available()) {
            int c = client.read(); if (c < 0) break;
            if (n < resp_max - 1) resp[n++] = (char)c;
            last = millis();
        }
        if (!client.connected() && !client.available()) break;
        delay(1);
    }
    client.stop(); resp[n] = 0; net_last_recv_len = n;
    if (!n) { net_last_err = NET_ERR_IO; return NET_ERR_IO; }
    char *body = strstr(resp, "\r\n\r\n");
    if (!body) return n;
    body += 4; int body_len = n - (int)(body - resp);
    memmove(resp, body, body_len); resp[body_len] = 0; return body_len;
}

extern "C" {

void net_poll(void) {}
bool net_available(void) { return inited; }
bool net_init(void) {
    if (!inited) { WiFi.mode(WIFI_STA); WiFi.setSleep(false); inited = true; }
    return true;
}
bool net_connect(const char *ssid, const char *pass, int timeout_ms) {
    if (!ssid || !net_init()) return false;
    WiFi.begin(ssid, pass ? pass : "");
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && (uint32_t)(millis() - start) < (uint32_t)timeout_ms) delay(25);
    return WiFi.status() == WL_CONNECTED;
}
void net_disconnect(void) { WiFi.disconnect(); }
bool net_is_connected(void) { return inited && WiFi.status() == WL_CONNECTED; }
bool net_ip_str(char *out, int maxlen) {
    if (!out || maxlen <= 0 || !net_is_connected()) return false;
    snprintf(out, maxlen, "%s", WiFi.localIP().toString().c_str()); return true;
}
int net_http_post(const char *host, uint16_t port, const char *path,
                  const char *body, char *resp, int resp_max, int timeout_ms) {
    if (!host || !path || !body) return NET_ERR_BODY;
    static char req[2048]; size_t blen = strlen(body);
    int n = snprintf(req, sizeof req, "POST %s HTTP/1.0\r\nHost: %s\r\nContent-Type: application/json\r\nContent-Length: %u\r\nConnection: close\r\n\r\n%s",
                     path, host, (unsigned)blen, body);
    if (n < 0 || n >= (int)sizeof req) return NET_ERR_BODY;
    return http_xact(host, port, req, resp, resp_max, timeout_ms);
}
int net_http_get(const char *host, uint16_t port, const char *path,
                 char *resp, int resp_max, int timeout_ms) {
    if (!host || !path) return NET_ERR_BODY;
    static char req[512]; int n = snprintf(req, sizeof req, "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n", path, host);
    if (n < 0 || n >= (int)sizeof req) return NET_ERR_BODY;
    return http_xact(host, port, req, resp, resp_max, timeout_ms);
}

}
