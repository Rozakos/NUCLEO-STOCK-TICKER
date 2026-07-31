/**
 * esp01.h  —  ESP-01 (ESP8266) WiFi co-processor driver over USART6.
 *
 * The ESP-01 runs stock Espressif AT firmware, which offers no PPP/SLIP
 * server, so it cannot be an LwIP netif. Instead it is a *socket provider*:
 * it owns WiFi association, DHCP, DNS and TCP, while TLS stays on the STM32
 * (mbedTLS is layered on top of esp01_send/esp01_recv exactly as it is on
 * LwIP sockets). See net_link.h for the abstraction that picks between this
 * and the Ethernet path.
 *
 * Wiring (STM32F746G-DISCO Arduino header, see AGENTS.md §8):
 *   ESP-01 RX  <- PC6 / Arduino D1 (USART6_TX)
 *   ESP-01 TX  -> PC7 / Arduino D0 (USART6_RX)
 *   ESP-01 CH_PD and RST pulled to 3V3; VCC needs its own 3.3 V supply.
 *
 * Threading: every AT exchange is serialised by an internal mutex, so the
 * net task (TLS client) and web task (HTTP server) may call in concurrently.
 * All calls block the caller for at most the documented timeout.
 */
#ifndef APP_ESP01_H
#define APP_ESP01_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Link ids exposed by AT+CIPMUX=1. Id 4 is reserved by the AT firmware for
 * the server's own listening socket, so only 0..3 are usable. */
#define ESP01_MAX_LINKS   4

/* Largest payload a single AT+CIPSEND accepts. */
#define ESP01_SEND_CHUNK  2048U

/* Return codes shared by the send/recv paths. */
#define ESP01_OK           0
#define ESP01_ERR         -1    /* hard failure: connection is dead        */
#define ESP01_WOULDBLOCK  -2    /* no data available yet, retry later      */

/** Bring up USART6 + the module: reset, ATE0, station mode, CIPMUX, and
 *  passive receive if the firmware supports it. Safe to call once at boot;
 *  returns false when no module answers (then WiFi is simply unavailable). */
bool esp01_init(void);

/** True once esp01_init() found a responsive module. */
bool esp01_present(void);

/** Associate with an access point and wait for a DHCP lease.
 *  Blocks up to ~20 s. Returns false on bad credentials or no lease. */
bool esp01_join(const char *ssid, const char *password);

/** True while associated and holding a non-zero IP. Cheap: reads cached
 *  state maintained from the module's WIFI CONNECTED/DISCONNECTED URCs. */
bool esp01_is_connected(void);

/** Copy the cached station IP / gateway / netmask / MAC as dotted strings.
 *  Any pointer may be NULL. Values are refreshed on join and on reconnect. */
void esp01_get_status(char *ip, size_t ip_size, char *gateway,
                      size_t gateway_size, char *netmask, size_t netmask_size,
                      char *mac, size_t mac_size);

/* ---- Outbound (client) connections -------------------------------------- */

/** Open a TCP connection to host:port. `host` may be a DNS name — the module
 *  resolves it. Returns a link id (0..3) or a negative ESP01_* code. */
int esp01_connect(const char *host, uint16_t port);

/** Send exactly `len` bytes, splitting into ESP01_SEND_CHUNK pieces.
 *  Returns the byte count on success, ESP01_ERR on failure. */
int esp01_send(int link, const uint8_t *data, size_t len);

/** Read up to `len` bytes. Returns the count (>0), ESP01_WOULDBLOCK when the
 *  peer has sent nothing yet, or ESP01_ERR once the connection is closed and
 *  drained. */
int esp01_recv(int link, uint8_t *data, size_t len);

/** Close a link id. Safe on an already-closed link. */
void esp01_close(int link);

/* ---- Inbound (server) connections ---------------------------------------- */

/** Start the AT server on `port` (AT+CIPSERVER=1,<port>). */
bool esp01_listen(uint16_t port);

/** Stop the AT server. Existing inbound links stay open until closed. */
void esp01_listen_stop(void);

/** Return the link id of a client that connected but has not been handed out
 *  yet, or a negative ESP01_* code if none is waiting. Non-blocking. */
int esp01_accept(void);

#endif /* APP_ESP01_H */
