#include "app/net_task.h"
#include "app/config.h"
#include "app/format.h"
#include "app/history_data.h"
#include "app/logo_cache.h"
#include "app/settings.h"
#include "app/stock_api.h"
#include "app/stock_data.h"

#include <stdio.h>
#include <string.h>

#include "lwip/ip4_addr.h"
#include "lwip/netif.h"

extern struct netif gnetif;

static void wait_for_refresh_or_settings(uint32_t delay_ms,
                                         uint32_t settings_seen)
{
  uint32_t start = osKernelSysTick();
  while ((osKernelSysTick() - start) < delay_ms &&
         settings_generation() == settings_seen &&
         !history_data_request_pending())
  {
    osDelay(250);
  }
}

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

  printf("[net] DHCP bound. IP  = %s\r\n", ip4addr_ntoa(netif_ip4_addr(&gnetif)));
  printf("[net]              GW  = %s\r\n", ip4addr_ntoa(netif_ip4_gw(&gnetif)));
  printf("[net]              MASK= %s\r\n", ip4addr_ntoa(netif_ip4_netmask(&gnetif)));
}

void StartNetTask(void const *argument)
{
  (void)argument;
  char symbols[APP_MAX_SYMBOLS][APP_SYMBOL_LENGTH];
  size_t symbol_count = settings_get_symbols(symbols);
  size_t symbol_index = 0;
  uint32_t settings_seen = settings_generation();

  net_wait_for_ip();
  stock_api_init();

  for (;;)
  {
    history_request_t history_request;
    if (history_data_take_request(&history_request))
    {
      history_snapshot_t history = { 0 };
      char error[96];
      printf("[history] fetching %s %s...\r\n", history_request.symbol,
             history_request.range);
      if (stock_api_fetch_history(&history_request, &history, error,
                                  sizeof(error)) == 0)
      {
        if (history_data_request_current(history_request.generation))
        {
          history_data_publish(&history);
          printf("[history] %s %s: %u points\r\n", history.symbol,
                 history.range, (unsigned)history.point_count);
        }
      }
      else
      {
        history_data_publish_error(&history_request, error);
        printf("[history] %s %s failed: %s\r\n", history_request.symbol,
               history_request.range, error);
      }
      continue;
    }

    if (settings_generation() != settings_seen)
    {
      symbol_count = settings_get_symbols(symbols);
      symbol_index = 0;
      settings_seen = settings_generation();
      stock_data_reset();
      printf("[stock] symbols updated from web UI\r\n");
    }

    stock_snapshot_t snapshot = { 0 };
    char error[96];
    const char *symbol = symbols[symbol_index];

    printf("[stock] fetching %s...\r\n", symbol);
    if (stock_api_fetch_quote(symbol, &snapshot, error, sizeof(error)) == 0)
    {
      char price[20];
      char change[20];
      format_decimal_2(price, sizeof(price), snapshot.last, 0);
      format_decimal_2(change, sizeof(change), snapshot.change_pct, 1);
      printf("[stock] %s %s (%s%%), %u spark points\r\n",
             snapshot.symbol, price, change,
             (unsigned)snapshot.close_count);
    }
    else
    {
      snprintf(snapshot.symbol, sizeof(snapshot.symbol), "%s", symbol);
      snprintf(snapshot.status, sizeof(snapshot.status), "%.47s", error);
      printf("[stock] %s fetch failed: %s\r\n", symbol, error);
    }
    stock_data_publish(&snapshot);

    /* Fetch each symbol's logo once (PNG -> SDRAM cache; ui_task displays it). */
    if (logo_cache_should_fetch(symbol))
    {
      const uint8_t *png = NULL;
      size_t png_len = 0;
      char logo_error[64];
      if (stock_api_fetch_logo(symbol, &png, &png_len, logo_error,
                               sizeof(logo_error)) == 0)
      {
        logo_cache_store(symbol, png, png_len);
        printf("[logo] %s cached (%u bytes)\r\n", symbol, (unsigned)png_len);
      }
      else
      {
        logo_cache_mark_failed(symbol);
        printf("[logo] %s logo failed: %s\r\n", symbol, logo_error);
      }
    }

    symbol_index = (symbol_index + 1U) % symbol_count;
    wait_for_refresh_or_settings(
        (settings_get_refresh_seconds() * 1000U) / symbol_count, settings_seen);
  }
}
