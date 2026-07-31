# NUCLEO-STOCK-TICKER — STM32F746G-DISCO Stock Ticker

A port of [Rozakos/CYD-Stock-Ticker](https://github.com/Rozakos/CYD-Stock-Ticker)
(ESP32 "Cheap Yellow Display") to the **STM32F746G-DISCO**, with a higher-resolution
display, **Ethernet + WiFi** networking and a **Bluetooth** console.

| | Source (CYD) | This port |
|---|---|---|
| MCU | ESP32 | STM32F746NG (Cortex-M7) |
| Display | 320×240 ST7789 | **480×272** RK043FN48H (onboard LTDC panel) |
| Touch | XPT2046 (resistive) | FT5336 (capacitive, I2C3) |
| UI | LVGL 9.x | LVGL 9.x |
| Net | WiFi | **Ethernet** (LAN8742A RMII) + LwIP, **WiFi** via ESP-01 as fallback |
| Bluetooth | — | **HC-05** SPP console, log mirror and price alerts |
| TLS | (ESP) | mbedTLS (pinned CA, session resumption, keep-alive) |
| JSON | ArduinoJson | cJSON |
| Data | self-hosted yfinance proxy (`rozakos.eu/stocks/api/v1`, bearer token) | same |

## Features

- **Live watchlist** (up to 8 symbols): price, day change %, gradient sparkline,
  API-fetched company logos. One batch HTTPS request per refresh cycle over a
  persistent TLS connection. Adaptive layout: ≤4 symbols one full-width column,
  5–8 two compact columns — never scrolls.
- **Extended hours (Revolut-style)**: during pre-market / after-hours the rows and
  detail header show the extended print and its change vs regular close, and the
  whole market screen shifts to a purple-tinted night palette.
- **Session status bar**: the title tracks the API's `market_state` — PREMARKET /
  MARKET OPEN / AFTER HOURS / MARKET CLOSED — with an amber sun while open and a
  crescent moon otherwise. The right side shows the portfolio total with day P/L
  (`$12,345 ▲ 1.23%`, green/red, extended-hours aware) and flips to an amber
  `stale Ns` warning if quote refreshes stop landing.
- **Detail screen** per symbol: smoothed gradient chart with price/date ticks,
  1D/1W/1M/6M/1Y/5Y/Max ranges, progressive 1D session chart, silent auto-refresh
  each interval. 1D change/color is anchored to the previous close (not the open).
- **Extended-hours 1D chart** (API `prepost=1`): the 1D axis spans the full
  04:00–20:00 ET window with faint divider lines at the regular open/close, and the
  middle time ticks snap to those dividers. Re-tapping the active 1D button opens a
  session-view dropdown — Full day / Pre-market / Live market / After hours — that
  only offers segments which already have data (inert during pre-market); segment
  views re-render from the fetched snapshot with no extra request.
- **Dual-link networking**: Ethernet is preferred whenever it has link and a DHCP
  lease; an **ESP-01** on USART6 provides WiFi as an automatic fallback. Both sit
  behind one TCP abstraction (`app/net_link.h`), so the HTTPS client *and* the web
  admin work over either — unplug the cable and quotes keep arriving. TLS always
  runs on the STM32, so the pinned CA and session resumption are unaffected by
  which link is carrying traffic. (The ESP-01 is a socket provider, not an LwIP
  netif: stock AT firmware has no PPP/SLIP server.)
- **Bluetooth console** via an **HC-05** on UART7: pair from a phone with any SPP
  terminal app for the same control the web admin offers — watchlist, shares,
  refresh interval — plus `status` and `list` with live prices. Needs no network,
  which is exactly when it is most useful.
- **Price alerts**: per-symbol above/below thresholds, edge-triggered against the
  extended-hours-aware price and pushed to the paired phone as they cross.
  Configurable from both the Bluetooth console and the web admin.
- **Debug log over Bluetooth**: the `printf` console is mirrored to the HC-05 so
  logs can be watched without the ST-Link cable (`log on|off` to toggle).
- **Web admin** at `http://<board-ip>/`: add/delete symbols, shares owned,
  refresh interval, price alert thresholds.
- **Persistent settings**: saved to a microSD card (`ticker.cfg`) when present,
  otherwise to **internal flash sector 7** (0x080C0000) — watchlist, shares and
  refresh interval survive power cycles with no SD card. Note: a flash save stalls
  the CPU ~1 s (sector erase, single-bank XIP); unchanged saves are skipped.
- **Tear-free display**: double-buffered SDRAM framebuffers; buffer swaps are queued
  in the LTDC shadow registers and applied by hardware in vertical blanking.
- Status bar: hand-drawn ethernet jack and WiFi icons — whichever link is carrying
  traffic lights green and the other dims; tap either for a connection-info popup.

## Build

- STM32CubeIDE 1.19.x, target **STM32F746G-DISCO**.
- Copy `Core/Inc/app/secrets.h.example` → `Core/Inc/app/secrets.h` and set your
  `STOCK_API_TOKEN`. `secrets.h` is git-ignored. To enable WiFi, also set
  `WIFI_SSID`/`WIFI_PASSWORD` there — left empty, the ESP-01 is never probed and
  the board runs Ethernet-only.
- Console: USART1 via the ST-Link VCP, **115200 8N1**.
- The linker caps the image at **768 KB**: flash sector 7 is reserved for the
  settings store. A build that outgrows it fails to link instead of corrupting
  saved settings. Currently ~708 KB (90%).

### Optional radio modules

Both are optional — the firmware runs fine with neither attached, and each is
independent of the other. Full wiring tables and firmware requirements are in
`AGENTS.md` §8 (ESP-01) and §9 (HC-05).

| | ESP-01 (WiFi) | HC-05 (Bluetooth) |
|---|---|---|
| Port | USART6 @ 115200 | UART7 @ 9600 (factory rate) |
| Pins | PC6/PC7 = Arduino **D1/D0** | PF7/PF6 = Arduino **A4/A5** |
| CubeMX | already in the `.ioc` | configured in app code, no `.ioc` change |
| Gotcha | needs its **own 3.3 V supply** (~300 mA TX spikes brown out the DISCO's 3V3 pin) and AT firmware ≥ 1.7 for `AT+CIPRECVMODE` | none; ~40 mA |

> **Status:** both drivers are code-complete but have **not yet been run on
> hardware**. See `AGENTS.md` §6 for bring-up steps.

## Milestones

- [x] CubeMX skeleton (ETH MAC, LTDC 480×272, SDRAM, DMA2D, SD, FreeRTOS, FATFS)
- [x] App config/secrets scaffolding, `printf`→USART1 console
- [x] **M1** Ethernet bring-up: LwIP + DHCP + DNS + TCP
- [x] **M2** mbedTLS HTTPS client → live JSON parsed (cJSON), pinned CA
- [x] **M3** LVGL on LTDC + DMA2D, FT5336 touch, double buffering
- [x] **M4** UI port: watchlist, sparklines, detail charts, logos, web admin
- [x] Extended-hours quotes + night theme, settings persistence (SD or flash)
- [x] 1D extended-hours chart (`prepost=1`): full-window axis, open/close dividers,
      session-view dropdown on the 1D button
- [~] **WiFi** via ESP-01 as a fallback link (code complete, not yet hardware-tested)
- [~] **Bluetooth** via HC-05: console, log mirror, price alerts (code complete,
      not yet hardware-tested)

## Hardware notes

- Framebuffers live in external SDRAM at `0xC0000000`/`0xC0080000` (FMC); TLS heap,
  HTTP buffer, LVGL heap and logo cache also sit in SDRAM.
- D-cache + MPU are configured for the ETH DMA descriptor/buffer region and SDRAM
  (layout cribbed from ST's `LwIP_HTTP_Server_Netconn_RTOS` example).
- Settings store: flash sector 7 (256 KB @ `0x080C0000`), magic + checksum guarded.
  The blob carries the watchlist, shares, refresh interval and alert thresholds; an
  older-format blob is migrated on load rather than discarded.
- UART map: **USART1** ST-Link console, **USART6** ESP-01, **UART7** HC-05. The two
  radio drivers own their own IRQ handlers and never block a caller — the ESP-01
  uses AT passive receive (no flow control on its 8-pin header), and the HC-05's
  transmit ring drops rather than stalls, since `printf` from every task feeds it.
