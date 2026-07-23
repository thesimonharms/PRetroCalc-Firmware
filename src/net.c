/* PRetroCalc OS - WiFi (CYW43) + minimal plain-HTTP client via lwIP raw API.
 * Poll-mode (NO_SYS): call net_poll() from the main loop. */
#include "net.h"
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "lwip/tcp.h"
#include "lwip/dns.h"
#include "lwip/ip_addr.h"
#include "lwip/netif.h"
#include <string.h>
#include <stdio.h>

static bool inited = false;
static bool connected = false;

void net_poll(void) {
    if (inited) cyw43_arch_poll();
}

bool net_available(void) { return inited; }

bool net_init(void) {
    if (inited) return true;
    if (cyw43_arch_init()) return false;
    cyw43_arch_enable_sta_mode();
    inited = true;
    return true;
}

bool net_connect(const char *ssid, const char *pass, int timeout_ms) {
    if (!inited && !net_init()) return false;
    int r = cyw43_arch_wifi_connect_timeout_ms(ssid, pass, CYW43_AUTH_WPA2_AES_PSK, timeout_ms);
    connected = (r == 0);
    return connected;
}

void net_disconnect(void) {
    if (inited) cyw43_arch_wifi_connect_timeout_ms("", "", 0, 100); /* best-effort */
    connected = false;
}

bool net_is_connected(void) { return connected; }

bool net_ip_str(char *out, int maxlen) {
    if (!connected) return false;
    const ip_addr_t *ip = netif_ip4_addr(&cyw43_state.netif[CYW43_ITF_STA]);
    snprintf(out, maxlen, "%s", ip4addr_ntoa(ip));
    return true;
}

/* ---------------- HTTP POST client ---------------- */

typedef struct {
    struct tcp_pcb *pcb;
    char *resp;
    int resp_max;
    int resp_len;
    bool done;
    bool err;
    ip_addr_t addr;
    bool dns_done;
    bool dns_ok;
    const char *request;
    int req_len;
    int req_sent;
} http_t;

static void dns_cb(const char *name, const ip_addr_t *ipaddr, void *arg) {
    http_t *h = arg;
    h->dns_done = true;
    if (ipaddr) { h->addr = *ipaddr; h->dns_ok = true; }
}

static err_t recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    http_t *h = arg;
    if (!p) { h->done = true; return ERR_OK; }
    int copy = p->tot_len;
    if (h->resp_len + copy > h->resp_max - 1) copy = h->resp_max - 1 - h->resp_len;
    if (copy > 0) {
        pbuf_copy_partial(p, h->resp + h->resp_len, copy, 0);
        h->resp_len += copy;
    }
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static err_t sent_cb(void *arg, struct tcp_pcb *pcb, u16_t len) {
    http_t *h = arg;
    if (h->req_sent < h->req_len) {
        int chunk = h->req_len - h->req_sent;
        if (chunk > tcp_sndbuf(pcb)) chunk = tcp_sndbuf(pcb);
        if (chunk > 0) {
            tcp_write(pcb, h->request + h->req_sent, chunk, TCP_WRITE_FLAG_COPY);
            tcp_output(pcb);
            h->req_sent += chunk;
        }
    }
    return ERR_OK;
}

static err_t connect_cb(void *arg, struct tcp_pcb *pcb, err_t err) {
    http_t *h = arg;
    if (err != ERR_OK) { h->err = true; return ERR_OK; }
    tcp_sent(pcb, sent_cb);
    tcp_recv(pcb, recv_cb);
    sent_cb(arg, pcb, 0);
    return ERR_OK;
}

static void err_cb(void *arg, err_t err) {
    http_t *h = arg;
    h->err = true; h->done = true;
}

int net_http_post(const char *host, uint16_t port, const char *path,
                  const char *body, char *resp, int resp_max, int timeout_ms) {
    if (!connected) return -1;
    resp[0] = 0;

    static http_t h;
    memset(&h, 0, sizeof h);
    h.resp = resp; h.resp_max = resp_max;

    /* build request */
    static char req[1536];
    int blen = strlen(body);
    int hostlen = strlen(host);
    if (blen + hostlen + 256 > (int)sizeof(req)) return -1;
    h.req_len = snprintf(req, sizeof req,
        "POST %s HTTP/1.0\r\nHost: %s\r\nContent-Type: application/json\r\n"
        "Content-Length: %d\r\nConnection: close\r\n\r\n%s",
        path, host, blen, body);
    h.request = req;

    /* resolve host (may be an IP literal) */
    ip_addr_t addr;
    if (ipaddr_aton(host, &addr)) {
        h.addr = addr; h.dns_done = true; h.dns_ok = true;
    } else {
        err_t e = dns_gethostbyname(host, &addr, dns_cb, &h);
        if (e == ERR_OK) { h.addr = addr; h.dns_done = true; h.dns_ok = true; }
        else if (e != ERR_INPROGRESS) return -1;
    }

    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
    while (!h.dns_done && !time_reached(deadline)) { net_poll(); sleep_ms(2); }
    if (!h.dns_ok) return -1;

    h.pcb = tcp_new();
    if (!h.pcb) return -1;
    tcp_arg(h.pcb, &h);
    tcp_err(h.pcb, err_cb);
    if (tcp_connect(h.pcb, &h.addr, port, connect_cb) != ERR_OK) { tcp_close(h.pcb); return -1; }

    while (!h.done && !h.err && !time_reached(deadline)) { net_poll(); sleep_ms(2); }
    if (h.pcb) { tcp_abort(h.pcb); h.pcb = NULL; }
    if (h.err || h.resp_len == 0) return -1;
    resp[h.resp_len] = 0;
    /* strip HTTP headers: body starts after the first blank line */
    char *body_start = strstr(resp, "\r\n\r\n");
    if (body_start) {
        body_start += 4;
        int blen = h.resp_len - (body_start - resp);
        memmove(resp, body_start, blen);
        resp[blen] = 0;
        return blen;
    }
    return h.resp_len;
}
