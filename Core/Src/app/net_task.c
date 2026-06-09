/**
 * net_task.c — M1 Ethernet bring-up.
 *
 * Sequence (all logged over USART1 @115200):
 *   1. wait for link up + DHCP-bound IPv4 address
 *   2. print IP / gateway / netmask
 *   3. resolve STOCK_API_HOST via DNS
 *   4. open a raw TCP connection to STOCK_API_PORT (443) and close it
 *
 * Proves the stack reaches the internet before mbedTLS (M2) is layered on.
 */
#include "app/net_task.h"
#include "app/config.h"

#include <stdio.h>
#include <string.h>

#include "lwip/netif.h"
#include "lwip/ip4_addr.h"
#include "lwip/api.h"        /* netconn_gethostbyname() */
#include "lwip/sockets.h"

/* Defined in LWIP/App/lwip.c */
extern struct netif gnetif;

/* Poll until the interface is up and DHCP has supplied a non-zero address. */
static void net_wait_for_ip(void)
{
  printf("[net] waiting for link + DHCP...\r\n");
  for (;;)
  {
    if (netif_is_up(&gnetif) && !ip4_addr_isany_val(*netif_ip4_addr(&gnetif)))
    {
      break;
    }
    osDelay(200);
  }

  /* ip4addr_ntoa uses a shared static buffer, so print one at a time. */
  printf("[net] DHCP bound. IP  = %s\r\n", ip4addr_ntoa(netif_ip4_addr(&gnetif)));
  printf("[net]              GW  = %s\r\n", ip4addr_ntoa(netif_ip4_gw(&gnetif)));
  printf("[net]              MASK= %s\r\n", ip4addr_ntoa(netif_ip4_netmask(&gnetif)));
}

/* Resolve `host` and attempt a TCP connect to `port`. Returns 0 on success. */
static int net_test_tcp(const char *host, uint16_t port)
{
  ip_addr_t addr;
  err_t e = netconn_gethostbyname(host, &addr);
  if (e != ERR_OK)
  {
    printf("[net] DNS resolve FAILED for %s (err=%d)\r\n", host, (int)e);
    return -1;
  }
  printf("[net] DNS %s -> %s\r\n", host, ipaddr_ntoa(&addr));

  int sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0)
  {
    printf("[net] socket() failed\r\n");
    return -1;
  }

  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = lwip_htons(port);
  sa.sin_addr.s_addr = ip_addr_get_ip4_u32(&addr);

  printf("[net] connecting to %s:%u ...\r\n", host, (unsigned)port);
  if (connect(sock, (struct sockaddr *)&sa, sizeof(sa)) != 0)
  {
    printf("[net] TCP connect FAILED\r\n");
    closesocket(sock);
    return -1;
  }

  printf("[net] TCP connect OK (stack reaches the API host)\r\n");
  closesocket(sock);
  return 0;
}

void StartNetTask(void const *argument)
{
  (void)argument;

  net_wait_for_ip();

  for (;;)
  {
    net_test_tcp(STOCK_API_HOST, STOCK_API_PORT);
    osDelay(10000);   /* repeat every 10 s during bring-up */
  }
}
