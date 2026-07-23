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
int net_last_recv_len = 0;
int net_last_err = 0;       /* lwIP err_t from the last err_cb (0 if none) */

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
    if (r != 0) { connected = false; return false; }
    /* wait for DHCP to assign an address (link up + IP != 0) */
    absolute_time_t deadline = make_timeout_time_ms(8000);
    for (;;) {
        net_poll();
        int link = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
        uint32_t ip = cyw43_state.netif[CYW43_ITF_STA].ip_addr.addr;
        if (link == CYW43_LINK_UP && ip != 0) break;
        if (time_reached(deadline)) break;
        sleep_ms(50);
    }
    connected = true;
    /* Disable CYW43 power-save for the session. The default PM2 mode lets the
     * radio sleep when there is no activity "for some time" -- which is exactly
     * the long, silent window while a LAN LLM generates its reply (Ollama with
     * stream:false sends nothing until the whole response is ready). PM2 sleep
     * makes the AP drop the association mid-generation -> err_cb with no data
     * received (the "connects but doesn't stay connected" symptom). */
    if (inited) cyw43_wifi_pm(&cyw43_state, CYW43_NONE_PM);
    return true;
}

void net_disconnect(void) {
    if (inited) {
        cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
        cyw43_wifi_pm(&cyw43_state, CYW43_DEFAULT_PM);   /* restore default PM */
    }
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
    bool got_data;
    bool connected;     /* connect_cb fired (server accepted the TCP connection) */
    int content_length;   /* from Content-Length header, -1 if unknown */
    int body_offset;      /* where body starts (after \r\n\r\n), -1 if not yet */
    ip_addr_t addr;
    bool dns_done;
    bool dns_ok;
    const char *request;
    int req_len;
    int req_sent;
} http_t;

/* case-insensitive substring search (newlib lacks strcasestr) */
static char *my_strcasestr(const char *hay, const char *needle) {
    if (!*needle) return (char *)hay;
    for (; *hay; hay++) {
        const char *a = hay, *b = needle;
        while (*a && *b && ((*a >= 'A' && *a <= 'Z' ? *a + 32 : *a) ==
                            (*b >= 'A' && *b <= 'Z' ? *b + 32 : *b))) { a++; b++; }
        if (!*b) return (char *)hay;
    }
    return NULL;
}

/* scan buffered response for end-of-headers and Content-Length */
static void parse_headers(http_t *h) {
    if (h->body_offset >= 0) return;
    h->resp[h->resp_len] = 0;
    char *be = strstr(h->resp, "\r\n\r\n");
    if (!be) return;
    h->body_offset = (be - h->resp) + 4;
    h->content_length = -1;
    char *cl = my_strcasestr(h->resp, "Content-Length:");
    if (cl && cl < be) h->content_length = atoi(cl + 15);
}

/* true once the full body has arrived */
static bool body_complete(http_t *h) {
    if (h->body_offset < 0 || h->content_length < 0) return false;
    return h->resp_len >= h->body_offset + h->content_length;
}

static void dns_cb(const char *name, const ip_addr_t *ipaddr, void *arg) {
    http_t *h = arg;
    h->dns_done = true;
    if (ipaddr) { h->addr = *ipaddr; h->dns_ok = true; }
}

static err_t recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    http_t *h = arg;
    if (!p) { h->done = true; return ERR_OK; }   /* server closed connection */
    int copy = p->tot_len;
    if (h->resp_len + copy > h->resp_max - 1) copy = h->resp_max - 1 - h->resp_len;
    if (copy > 0) {
        pbuf_copy_partial(p, h->resp + h->resp_len, copy, 0);
        h->resp_len += copy;
    }
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    if (h->resp_len > 0) h->got_data = true;
    parse_headers(h);
    if (body_complete(h)) h->done = true;      /* full body received; stop early */
    return ERR_OK;
}

static err_t sent_cb(void *arg, struct tcp_pcb *pcb, u16_t len) {
    http_t *h = arg;
    while (h->req_sent < h->req_len) {
        int chunk = h->req_len - h->req_sent;
        int avail = tcp_sndbuf(pcb);
        if (avail <= 0) break;                 /* wait for next sent_cb when space frees */
        if (chunk > avail) chunk = avail;
        err_t e = tcp_write(pcb, h->request + h->req_sent, chunk, TCP_WRITE_FLAG_COPY);
        if (e != ERR_OK) break;                /* mem err etc; retry on next callback */
        h->req_sent += chunk;
    }
    tcp_output(pcb);
    return ERR_OK;
}

static err_t connect_cb(void *arg, struct tcp_pcb *pcb, err_t err) {
    http_t *h = arg;
    if (err != ERR_OK) { h->err = true; return ERR_OK; }
    h->connected = true;
    tcp_sent(pcb, sent_cb);
    tcp_recv(pcb, recv_cb);
    sent_cb(arg, pcb, 0);
    return ERR_OK;
}

/* lwIP calls err_cb when the connection is torn down abnormally (RST, timeout,
 * fatal error). IMPORTANT: in the raw API the pcb has already been freed by the
 * time err_cb fires, so we must NOT touch it -- and we must clear it so the
 * outer loop never calls tcp_abort() on the freed pcb (use-after-free). */
static void err_cb(void *arg, err_t err) {
    http_t *h = arg;
    h->err = true; h->done = true;
    h->pcb = NULL;          /* already freed by lwIP; do not abort again */
    net_last_err = err;
}

/* error codes for diagnosis */
#define NET_OK          0
#define NET_ERR_NOTCONN -1
#define NET_ERR_BODY    -2
#define NET_ERR_DNS     -3
#define NET_ERR_PCB     -4
#define NET_ERR_CONNECT -5
#define NET_ERR_IO      -6

int net_http_post(const char *host, uint16_t port, const char *path,
                  const char *body, char *resp, int resp_max, int timeout_ms) {
    net_last_recv_len = 0;
    net_last_err = 0;
    if (!connected) return NET_ERR_NOTCONN;
    resp[0] = 0;

    static http_t h;
    memset(&h, 0, sizeof h);
    h.content_length = -1;   /* unknown until parse_headers finds the header */
    h.body_offset = -1;       /* no header/body boundary seen yet */
    h.resp = resp; h.resp_max = resp_max;

    /* build request */
    static char req[2048];
    int blen = strlen(body);
    int hostlen = strlen(host);
    if (blen + hostlen + 256 > (int)sizeof(req)) return NET_ERR_BODY;
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
        else if (e != ERR_INPROGRESS) return NET_ERR_DNS;
    }

    absolute_time_t deadline = make_timeout_time_ms(timeout_ms);
    while (!h.dns_done && !time_reached(deadline)) { net_poll(); sleep_ms(2); }
    if (!h.dns_ok) return NET_ERR_DNS;

    h.pcb = tcp_new();
    if (!h.pcb) return NET_ERR_PCB;
    tcp_arg(h.pcb, &h);
    tcp_err(h.pcb, err_cb);
    err_t ce = tcp_connect(h.pcb, &h.addr, port, connect_cb);
    if (ce != ERR_OK) { tcp_close(h.pcb); return NET_ERR_CONNECT; }

    /* wait until full body received (done), error, or timeout.
     * done is set when: server closes cleanly OR body_complete(). */
    while (!h.err && !h.done && !time_reached(deadline)) {
        net_poll();
        sleep_ms(2);
    }
    /* if server closed without a Content-Length (chunked/HTTP1.0 close), we
     * may still have a complete body; accept whatever arrived. */
    if (h.pcb) { tcp_abort(h.pcb); h.pcb = NULL; }
    net_last_recv_len = h.resp_len;
    if (h.err && h.resp_len == 0) return NET_ERR_IO;
    if (h.resp_len == 0) return NET_ERR_IO;
    resp[h.resp_len] = 0;
    /* strip HTTP headers: body starts after the first blank line */
    char *body_start = strstr(resp, "\r\n\r\n");
    if (body_start) {
        body_start += 4;
        int bl = h.resp_len - (body_start - resp);
        memmove(resp, body_start, bl);
        resp[bl] = 0;
        return bl;
    }
    return h.resp_len;
}
