/**
 * net_link.c  —  Ethernet/WiFi link selection and a common TCP API.
 *
 * See net_link.h for the contract. The two implementations behind each call
 * are genuinely different beasts: the Ethernet path is a thin wrapper over
 * LwIP BSD sockets, while the WiFi path marshals AT commands to the ESP-01,
 * whose receive side is a poll (the module buffers, we ask). The polling loop
 * in wifi_recv() is what lets the ESP-01 present the same blocking-with-
 * timeout semantics that mbedTLS already expects from SO_RCVTIMEO.
 */
#include "app/net_link.h"

#include "app/config.h"
#include "app/esp01.h"

#include <stdio.h>
#include <string.h>

#include "cmsis_os.h"

#include "lwip/api.h"
#include "lwip/dns.h"
#include "lwip/inet.h"
#include "lwip/netif.h"
#include "lwip/sockets.h"

extern struct netif gnetif;

/* secrets.h is git-ignored, so a checkout that predates WiFi support simply
 * has no credentials - treat that as "Ethernet only" rather than failing. */
#ifndef WIFI_SSID
#define WIFI_SSID      ""
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD  ""
#endif

#define DEFAULT_RECV_TIMEOUT_MS  12000U

static bool wifi_enabled;

/* ---- Link selection ------------------------------------------------------ */

static bool eth_ready(void)
{
  return netif_is_link_up(&gnetif) &&
         !ip4_addr_isany_val(*netif_ip4_addr(&gnetif));
}

net_link_kind_t net_link_active(void)
{
  if (eth_ready()) return NET_LINK_ETH;
  if (wifi_enabled && esp01_is_connected()) return NET_LINK_WIFI;
  return NET_LINK_NONE;
}

const char *net_link_name(net_link_kind_t kind)
{
  switch (kind)
  {
    case NET_LINK_ETH:  return "ethernet";
    case NET_LINK_WIFI: return "wifi";
    default:            return "offline";
  }
}

net_link_kind_t net_link_wait_ready(uint32_t timeout_ms)
{
  uint32_t start = osKernelSysTick();
  for (;;)
  {
    net_link_kind_t kind = net_link_active();
    if (kind != NET_LINK_NONE) return kind;
    if ((osKernelSysTick() - start) >= timeout_ms) return NET_LINK_NONE;
    osDelay(200);
  }
}

void net_link_get_status(net_link_status_t *status)
{
  if (status == NULL) return;
  memset(status, 0, sizeof(*status));

  status->kind = net_link_active();

  if (status->kind == NET_LINK_WIFI)
  {
    esp01_get_status(status->ip, sizeof(status->ip), status->gateway,
                     sizeof(status->gateway), status->netmask,
                     sizeof(status->netmask), status->mac,
                     sizeof(status->mac));
    status->link_up = true;
    status->bound = (strcmp(status->ip, "0.0.0.0") != 0);
    /* The module owns DNS; it does not report the server it uses. */
    snprintf(status->dns, sizeof(status->dns), "%s", "via ESP");
    return;
  }

  /* Ethernet - also the shape reported while offline, so the popup keeps
   * showing the wired details the user expects to troubleshoot. */
  status->link_up = netif_is_link_up(&gnetif);
  status->bound = !ip4_addr_isany_val(*netif_ip4_addr(&gnetif));

  /* ip4addr_ntoa() shares one static buffer - the _r forms are required. */
  ip4addr_ntoa_r(netif_ip4_addr(&gnetif), status->ip, sizeof(status->ip));
  ip4addr_ntoa_r(netif_ip4_gw(&gnetif), status->gateway,
                 sizeof(status->gateway));
  ip4addr_ntoa_r(netif_ip4_netmask(&gnetif), status->netmask,
                 sizeof(status->netmask));

  const ip_addr_t *server = dns_getserver(0);
  if (server != NULL && !ip_addr_isany(server))
  {
    ipaddr_ntoa_r(server, status->dns, sizeof(status->dns));
  }
  else
  {
    snprintf(status->dns, sizeof(status->dns), "%s", "-");
  }

  snprintf(status->mac, sizeof(status->mac), "%02X:%02X:%02X:%02X:%02X:%02X",
           gnetif.hwaddr[0], gnetif.hwaddr[1], gnetif.hwaddr[2],
           gnetif.hwaddr[3], gnetif.hwaddr[4], gnetif.hwaddr[5]);
}

/* ---- Ethernet implementation --------------------------------------------- */

static void eth_apply_timeout(net_conn_t *conn)
{
  if (conn->applied_ms == conn->timeout_ms) return;
  struct timeval timeout;
  timeout.tv_sec = (long)(conn->timeout_ms / 1000U);
  timeout.tv_usec = (long)((conn->timeout_ms % 1000U) * 1000U);
  setsockopt(conn->id, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(conn->id, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  conn->applied_ms = conn->timeout_ms;
}

static int eth_open(net_conn_t *conn, const char *host, uint16_t port)
{
  ip_addr_t address;
  if (netconn_gethostbyname(host, &address) != ERR_OK) return NET_CONN_ERR;

  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return NET_CONN_ERR;

  conn->kind = NET_LINK_ETH;
  conn->id = fd;
  conn->timeout_ms = DEFAULT_RECV_TIMEOUT_MS;
  conn->applied_ms = 0;
  eth_apply_timeout(conn);

  struct sockaddr_in server = { 0 };
  server.sin_family = AF_INET;
  server.sin_port = lwip_htons(port);
  server.sin_addr.s_addr = ip_addr_get_ip4_u32(&address);
  if (connect(fd, (struct sockaddr *)&server, sizeof(server)) != 0)
  {
    closesocket(fd);
    conn->kind = NET_LINK_NONE;
    conn->id = -1;
    return NET_CONN_ERR;
  }

  return 0;
}

/* ---- WiFi implementation -------------------------------------------------- */

static int wifi_open(net_conn_t *conn, const char *host, uint16_t port)
{
  int link = esp01_connect(host, port);
  if (link < 0) return NET_CONN_ERR;

  conn->kind = NET_LINK_WIFI;
  conn->id = link;
  conn->timeout_ms = DEFAULT_RECV_TIMEOUT_MS;
  conn->applied_ms = conn->timeout_ms;
  return 0;
}

/* Turn the module's "ask and maybe get nothing" model into a blocking read.
 * The 5 ms backoff keeps a quiet connection from monopolising the AT channel
 * that the other task also needs. */
static int wifi_recv(net_conn_t *conn, uint8_t *data, size_t len)
{
  uint32_t start = osKernelSysTick();
  for (;;)
  {
    int result = esp01_recv(conn->id, data, len);
    if (result > 0) return result;
    if (result == ESP01_ERR) return NET_CONN_ERR;

    if ((osKernelSysTick() - start) >= conn->timeout_ms)
    {
      return NET_CONN_WOULDBLOCK;
    }
    osDelay(5);
  }
}

/* ---- Public connection API ------------------------------------------------ */

int net_conn_open(net_conn_t *conn, const char *host, uint16_t port)
{
  if (conn == NULL) return NET_CONN_ERR;
  memset(conn, 0, sizeof(*conn));
  conn->id = -1;

  switch (net_link_active())
  {
    case NET_LINK_ETH:  return eth_open(conn, host, port);
    case NET_LINK_WIFI: return wifi_open(conn, host, port);
    default:            return NET_CONN_ERR;
  }
}

int net_conn_send(net_conn_t *conn, const uint8_t *data, size_t len)
{
  if (conn == NULL || conn->id < 0) return NET_CONN_ERR;

  if (conn->kind == NET_LINK_WIFI)
  {
    int result = esp01_send(conn->id, data, len);
    return (result < 0) ? NET_CONN_ERR : result;
  }

  eth_apply_timeout(conn);
  int result = send(conn->id, data, len, 0);
  return (result < 0) ? NET_CONN_ERR : result;
}

int net_conn_recv(net_conn_t *conn, uint8_t *data, size_t len)
{
  if (conn == NULL || conn->id < 0) return NET_CONN_ERR;

  if (conn->kind == NET_LINK_WIFI) return wifi_recv(conn, data, len);

  eth_apply_timeout(conn);
  int result = recv(conn->id, data, len, 0);
  if (result > 0) return result;
  if (result == 0) return NET_CONN_ERR;              /* orderly shutdown */
  if (errno == EWOULDBLOCK || errno == EAGAIN) return NET_CONN_WOULDBLOCK;
  return NET_CONN_ERR;
}

void net_conn_close(net_conn_t *conn)
{
  if (conn == NULL || conn->id < 0) return;

  if (conn->kind == NET_LINK_WIFI)
  {
    esp01_close(conn->id);
  }
  else
  {
    closesocket(conn->id);
  }

  conn->id = -1;
  conn->kind = NET_LINK_NONE;
}

bool net_conn_is_open(const net_conn_t *conn)
{
  return conn != NULL && conn->id >= 0;
}

/* ---- Public server API ---------------------------------------------------- */

int net_server_open(net_server_t *server, uint16_t port)
{
  if (server == NULL) return NET_CONN_ERR;
  memset(server, 0, sizeof(*server));
  server->id = -1;
  server->port = port;
  server->kind = net_link_active();

  if (server->kind == NET_LINK_WIFI)
  {
    if (!esp01_listen(port))
    {
      server->kind = NET_LINK_NONE;
      return NET_CONN_ERR;
    }
    return 0;
  }

  if (server->kind != NET_LINK_ETH) return NET_CONN_ERR;

  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
  {
    server->kind = NET_LINK_NONE;
    return NET_CONN_ERR;
  }

  struct sockaddr_in address = { 0 };
  address.sin_family = AF_INET;
  address.sin_port = lwip_htons(port);
  address.sin_addr.s_addr = PP_HTONL(INADDR_ANY);

  if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
      listen(fd, 2) != 0)
  {
    closesocket(fd);
    server->kind = NET_LINK_NONE;
    return NET_CONN_ERR;
  }

  server->id = fd;
  return 0;
}

int net_server_accept(net_server_t *server, net_conn_t *conn,
                      uint32_t timeout_ms)
{
  if (server == NULL || conn == NULL) return NET_CONN_ERR;
  memset(conn, 0, sizeof(*conn));
  conn->id = -1;

  if (server->kind == NET_LINK_WIFI)
  {
    /* The AT server hands connections over as URCs; poll for one. */
    uint32_t start = osKernelSysTick();
    for (;;)
    {
      int link = esp01_accept();
      if (link >= 0)
      {
        conn->kind = NET_LINK_WIFI;
        conn->id = link;
        conn->timeout_ms = DEFAULT_RECV_TIMEOUT_MS;
        conn->applied_ms = conn->timeout_ms;
        return 0;
      }
      if ((osKernelSysTick() - start) >= timeout_ms) return NET_CONN_WOULDBLOCK;
      osDelay(20);
    }
  }

  if (server->kind != NET_LINK_ETH || server->id < 0) return NET_CONN_ERR;

  /* A finite accept timeout is what lets the web task notice that the
   * active link changed underneath it. */
  struct timeval timeout;
  timeout.tv_sec = (long)(timeout_ms / 1000U);
  timeout.tv_usec = (long)((timeout_ms % 1000U) * 1000U);
  setsockopt(server->id, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

  int fd = accept(server->id, NULL, NULL);
  if (fd < 0) return NET_CONN_WOULDBLOCK;

  conn->kind = NET_LINK_ETH;
  conn->id = fd;
  conn->timeout_ms = DEFAULT_RECV_TIMEOUT_MS;
  conn->applied_ms = 0;
  eth_apply_timeout(conn);
  return 0;
}

void net_server_close(net_server_t *server)
{
  if (server == NULL) return;

  if (server->kind == NET_LINK_WIFI)
  {
    esp01_listen_stop();
  }
  else if (server->id >= 0)
  {
    closesocket(server->id);
  }

  server->id = -1;
  server->kind = NET_LINK_NONE;
}

/* ---- Supervisor ----------------------------------------------------------- */

static void StartLinkTask(void const *argument)
{
  (void)argument;

  if (WIFI_SSID[0] == '\0')
  {
    printf("[link] no WIFI_SSID in secrets.h - ethernet only\r\n");
    osThreadTerminate(NULL);
    return;
  }

  if (!esp01_init())
  {
    printf("[link] ESP-01 unavailable - ethernet only\r\n");
    osThreadTerminate(NULL);
    return;
  }

  wifi_enabled = true;

  uint32_t backoff_ms = 5000U;
  for (;;)
  {
    if (!esp01_is_connected())
    {
      /* Keep WiFi warm even while Ethernet is carrying traffic, so an
       * unplugged cable fails over immediately instead of stalling for the
       * ~10 s an association takes. */
      if (esp01_join(WIFI_SSID, WIFI_PASSWORD))
      {
        backoff_ms = 5000U;
        printf("[link] wifi up; active link = %s\r\n",
               net_link_name(net_link_active()));
      }
      else
      {
        osDelay(backoff_ms);
        if (backoff_ms < 60000U) backoff_ms *= 2U;
      }
    }

    osDelay(2000);
  }
}

void net_link_start(void)
{
  osThreadDef(linkTask, StartLinkTask, osPriorityLow, 0, 1024);
  if (osThreadCreate(osThread(linkTask), NULL) == NULL)
  {
    printf("[link] failed to start supervisor task\r\n");
  }
}
