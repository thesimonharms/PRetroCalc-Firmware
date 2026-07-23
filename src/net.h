#ifndef NET_H
#define NET_H

#include <stdint.h>
#include <stdbool.h>

/* WiFi + minimal HTTP-over-TCP client (plain HTTP, for LAN LLM servers like
 * Ollama / llama.cpp / text-generation-webui which don't require TLS).
 * HTTPS to public LLM APIs is not supported (no TLS stack). */

bool net_init(void);                       /* power up CYW43 (once) */
bool net_available(void);
bool net_connect(const char *ssid, const char *pass, int timeout_ms);
void net_disconnect(void);
bool net_is_connected(void);
bool net_ip_str(char *out, int maxlen);    /* "192.168.1.42" */

/* Blocking HTTP POST to http://host:port/path with a JSON body.
 * Writes the response body into resp (NUL-terminated, up to resp_max).
 * Returns response length, or -1 on error. Calls net_poll internally. */
int  net_http_post(const char *host, uint16_t port, const char *path,
                   const char *body, char *resp, int resp_max, int timeout_ms);

/* must be called periodically from the main loop (lwIP poll mode) */
void net_poll(void);

#endif
