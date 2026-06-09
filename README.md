# NUCLEO-STOCK-TICKER — STM32F746G-DISCO Stock Ticker

A port of [Rozakos/CYD-Stock-Ticker](https://github.com/Rozakos/CYD-Stock-Ticker)
(ESP32 "Cheap Yellow Display") to the **STM32F746G-DISCO**, with a higher-resolution
display and **Ethernet** networking (WiFi planned later).

| | Source (CYD) | This port |
|---|---|---|
| MCU | ESP32 | STM32F746NG (Cortex-M7) |
| Display | 320×240 ST7789 | **480×272** RK043FN48H (onboard LTDC panel) |
| Touch | XPT2046 (resistive) | FT5336 (capacitive, I2C3) |
| UI | LVGL 9.x | LVGL 9.x |
| Net | WiFi | **Ethernet** (LAN8742A RMII) + LwIP |
| TLS | (ESP) | mbedTLS |
| JSON | ArduinoJson | cJSON |
| Data | self-hosted yfinance proxy (`rozakos.eu/stocks/api/v1`, bearer token) | same |

## Status

Work in progress. Milestones:

- [x] CubeMX skeleton (ETH MAC, LTDC 480×272, SDRAM, DMA2D, SD, FreeRTOS, FATFS)
- [x] App config/secrets scaffolding, `printf`→USART1 console
- [ ] **M1** Ethernet bring-up: LwIP + DHCP + DNS + TCP (proven over UART)
- [ ] **M2** mbedTLS HTTPS client → live JSON parsed (cJSON)
- [ ] **M3** LVGL on LTDC + DMA2D, FT5336 touch
- [ ] **M4** UI port: price/change, sparkline, trend screen, SD-card logos
- [ ] Later: web admin, WiFi

## Build

- STM32CubeIDE 1.19.x, target **STM32F746G-DISCO**.
- Copy `Core/Inc/app/secrets.h.example` → `Core/Inc/app/secrets.h` and set your
  `STOCK_API_TOKEN`. `secrets.h` is git-ignored.
- Console: USART1 via the ST-Link VCP, **115200 8N1**.

## Hardware notes

- Framebuffer lives in external SDRAM at `0xC0000000` (FMC).
- D-cache + MPU must be configured for the ETH DMA descriptor/buffer region and
  the SDRAM framebuffer (cribbed from ST's `LwIP_HTTP_Server_Netconn_RTOS` example).
