/**
 * config.h  —  non-secret application configuration for the STM32 stock ticker.
 *
 * Port of Rozakos/CYD-Stock-Ticker to the STM32F746G-DISCO.
 * Secret values (API token, host) live in secrets.h (git-ignored).
 */
#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include "app/secrets.h"

/* ---- Display (onboard RK043FN48H-CT672B panel) ------------------------- */
#define LCD_WIDTH             480
#define LCD_HEIGHT            272
/* LTDC layer-0 framebuffer base (external SDRAM via FMC). */
#define LCD_FB_BASE_ADDR      0xC0000000U

/* ---- Symbols to track (compile-time for now; runtime/web admin later) -- */
#define STOCK_SYMBOLS         { "AMD", "NVDA", "AAPL", "MSFT", "TSLA" }
#define STOCK_SYMBOL_COUNT    5

/* Default history range for the trend screen: 1D 1W 1M 6M 1Y 5Y Max */
#define STOCK_DEFAULT_RANGE   "1D"

/* ---- Refresh cadence ---------------------------------------------------- */
#define STOCK_REFRESH_MS      (60U * 1000U)   /* poll each symbol every 60 s */

/* ---- Networking --------------------------------------------------------- */
/* Use DHCP (1) or the static config below (0). */
#define NET_USE_DHCP          1
/* Static fallback (only used when NET_USE_DHCP == 0). */
#define NET_STATIC_IP         "192.168.1.50"
#define NET_STATIC_NETMASK    "255.255.255.0"
#define NET_STATIC_GW         "192.168.1.1"

/* ---- TLS ---------------------------------------------------------------- */
/* 1 = skip server-cert verification (INSECURE, bring-up only).
 * 0 = verify against the pinned CA in tls_ca_cert.c (preferred). */
#define TLS_INSECURE_SKIP_VERIFY  1

/* ---- Console ------------------------------------------------------------ */
/* printf() is retargeted to USART1 (ST-Link VCP) at 115200 8N1. */

#endif /* APP_CONFIG_H */
