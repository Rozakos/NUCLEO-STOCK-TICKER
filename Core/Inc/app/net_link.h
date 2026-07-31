/**
 * net_link.h  —  one TCP API over two very different links.
 *
 * The board can reach the network two ways:
 *   NET_LINK_ETH   LAN8742A + LwIP, sockets in firmware (fast, preferred)
 *   NET_LINK_WIFI  ESP-01 running AT firmware, sockets in the module
 *
 * mbedTLS, the stock API client and the web admin server all sit on top of
 * this, so TLS, certificate pinning and the HTTP code are identical either
 * way — only the bytes' route off the board changes.
 *
 * Policy: Ethernet wins whenever it has a link and a lease; WiFi is the
 * fallback. The supervisor task re-evaluates continuously, so unplugging the
 * cable migrates to WiFi and plugging it back migrates home. Callers notice a
 * switch by watching net_link_active() and rebuilding their connections.
 */
#ifndef APP_NET_LINK_H
#define APP_NET_LINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum
{
  NET_LINK_NONE = 0,
  NET_LINK_ETH,
  NET_LINK_WIFI
} net_link_kind_t;

/* Result codes for the send/recv calls. */
#define NET_CONN_ERR         -1   /* connection is dead                  */
#define NET_CONN_WOULDBLOCK  -2   /* nothing yet, and the timeout elapsed */

/** An open TCP connection on whichever link created it. */
typedef struct
{
  net_link_kind_t kind;
  int id;                 /* LwIP fd, or ESP-01 link id */
  uint32_t timeout_ms;    /* per-recv blocking budget; callers may change it
                           * any time and the next recv picks it up */
  uint32_t applied_ms;    /* internal: timeout currently set on the socket */
} net_conn_t;

/** A listening socket. */
typedef struct
{
  net_link_kind_t kind;
  int id;                 /* LwIP fd; unused for WiFi (the AT server is
                           * implicit once AT+CIPSERVER is on) */
  uint16_t port;
} net_server_t;

typedef struct
{
  net_link_kind_t kind;
  bool link_up;
  bool bound;
  char ip[16];
  char gateway[16];
  char netmask[16];
  char dns[16];
  char mac[18];
} net_link_status_t;

/** Start the link supervisor task. Call once, after the RTOS is running.
 *  Brings up the ESP-01 and joins WiFi in the background; Ethernet needs no
 *  help here (LwIP already runs). */
void net_link_start(void);

/** Which link should carry traffic right now. */
net_link_kind_t net_link_active(void);

/** Block until some link is usable, or `timeout_ms` elapses.
 *  Returns the link that came up, or NET_LINK_NONE on timeout. */
net_link_kind_t net_link_wait_ready(uint32_t timeout_ms);

/** Snapshot of the active link for the UI status bar / network popup. */
void net_link_get_status(net_link_status_t *status);

/** Human-readable name, e.g. for log lines. */
const char *net_link_name(net_link_kind_t kind);

/* ---- Client ---------------------------------------------------------- */

/** Resolve `host` and open a TCP connection on the active link.
 *  Returns 0 and fills `conn`, or NET_CONN_ERR. */
int net_conn_open(net_conn_t *conn, const char *host, uint16_t port);

/** Send exactly `len` bytes. Returns bytes written or NET_CONN_ERR. */
int net_conn_send(net_conn_t *conn, const uint8_t *data, size_t len);

/** Read up to `len` bytes, blocking up to conn->timeout_ms.
 *  Returns the count (>0), NET_CONN_WOULDBLOCK on timeout, NET_CONN_ERR
 *  once the peer has closed and the buffer is drained. */
int net_conn_recv(net_conn_t *conn, uint8_t *data, size_t len);

/** Close and invalidate `conn`. Safe on an already-closed connection. */
void net_conn_close(net_conn_t *conn);

/** True once the connection has been closed or was never opened. */
bool net_conn_is_open(const net_conn_t *conn);

/* ---- Server ---------------------------------------------------------- */

/** Listen on `port` using the active link. Returns 0 or NET_CONN_ERR. */
int net_server_open(net_server_t *server, uint16_t port);

/** Wait up to `timeout_ms` for a client. Returns 0 and fills `conn`, or
 *  NET_CONN_WOULDBLOCK when nobody arrived. */
int net_server_accept(net_server_t *server, net_conn_t *conn,
                      uint32_t timeout_ms);

/** Stop listening. */
void net_server_close(net_server_t *server);

#endif /* APP_NET_LINK_H */
