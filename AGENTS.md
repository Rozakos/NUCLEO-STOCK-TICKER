# AGENTS.md — context & handoff for AI coding agents

This file lets any AI agent (Claude, Codex, DeepSeek, …) resume work with full context.
**Keep it updated at the end of every working session.**

> Change history note: the initial port scaffolding (git setup, app config/secrets,
> printf console, README, this file) was authored by **Claude (Opus 4.8) via Claude Code**.
> Subsequent agents: please sign your significant changes in the "Session log" below.

---

## 1. What we're building

Porting [Rozakos/CYD-Stock-Ticker](https://github.com/Rozakos/CYD-Stock-Ticker) (ESP32
"Cheap Yellow Display" stock ticker) to the **STM32F746G-DISCO**.

- Display: onboard **480×272** RK043FN48H panel (LTDC), framebuffer in SDRAM @ `0xC0000000`.
- Touch: **FT5336** capacitive over **I2C3** (source used XPT2046 resistive).
- UI: **LVGL 9.x** (same as source).
- Network: **Ethernet** (LAN8742A RMII) + **LwIP**, then **mbedTLS** for HTTPS.
  WiFi is a later milestone.
- JSON: **cJSON** (source used ArduinoJson).
- Data: self-hosted yfinance proxy `https://rozakos.eu/stocks/api/v1`, bearer-token auth.
  Endpoints: `GET /stock/{symbol}` and `GET /history/{symbol}?range=1D|1W|1M|6M|1Y|5Y|Max`.

## 2. Hardware / toolchain facts

- MCU: STM32F746NGHx (Cortex-M7, D-cache ON). Board: STM32F746G-DISCO.
- IDE: **STM32CubeIDE 1.19.0**; project `.ioc` = `NUCLEO-STOCK-TICKER.ioc`.
- Local Cube FW package (reference source for vendoring): `~/STM32Cube/Repository/STM32Cube_FW_F7_V1.17.4`.
  - LwIP + mbedTLS middleware under `Middlewares/Third_Party/`.
  - Disco BSP under `Drivers/BSP/STM32746G-Discovery` (LCD/SDRAM/TS).
  - Reference net example: `Projects/STM32746G-Discovery/Applications/LwIP/LwIP_HTTP_Server_Netconn_RTOS`
    (its `ethernetif.c` uses the **new** ETH HAL API — matches this project).
- Console: **printf → USART1** (ST-Link VCP), **115200 8N1**. Retarget = `__io_putchar()` in `main.c` (USER CODE 4).
- ETH HAL: project uses the **new** API (`ETH_TxPacketConfig`, `DMARxDscrTab[ETH_RX_DESC_CNT]`,
  `HAL_ETH_ReadData`). `main.c` owns `heth`, descriptors, `MX_ETH_Init()`.
- Build system: STM32CubeIDE compiles everything under source-folder roots
  `Core`, `Drivers`, `Middlewares`, `FATFS`, `USB_HOST` (see `.cproject` `<sourceEntries>`).
  **New middleware → drop under `Middlewares/Third_Party/...` AND add include paths to
  `.cproject`** (both Debug and Release configs).

## 3. Key decisions already made (do not re-litigate without reason)

1. UI library = **LVGL 9.x**.
2. Networking = **LwIP + mbedTLS**, HTTPS direct to the API.
3. Data source = author's **rozakos.eu** API + user's own bearer token.
4. Sequencing = **Ethernet first**, then display/UI.
5. TCP/IP stack is brought in via **CubeMX** (enable LwIP in the `.ioc` and regenerate) —
   NOT hand-vendored — because `main.c` already owns the ETH handle/descriptors and CubeMX
   generates a matched `ethernetif.c`/`lwip.c` + patches linker & MPU. Same plan for mbedTLS.
6. TLS: start with `TLS_INSECURE_SKIP_VERIFY=1` (bring-up), then pin the CA.

## 4. Layout / conventions

- App code lives under `Core/Src/app/*.c` and `Core/Inc/app/*.h`; include as `#include "app/xxx.h"`
  (`../Core/Inc` is already an include path).
- Secrets in `Core/Inc/app/secrets.h` (**git-ignored**); template `secrets.h.example`. Non-secret
  tunables in `Core/Inc/app/config.h`.
- Edit generated files (`main.c`, `freertos.c`, etc.) ONLY inside `/* USER CODE BEGIN/END */`
  blocks so CubeMX regeneration preserves them.
- Commit style: imperative subject, body explains what/why, end with
  `Co-Authored-By:` for the agent. `secrets.h`, build output (`Debug/`,`Release/`) are git-ignored.
- Git: branch `main`, remote `origin` = `git@github.com:Rozakos/NUCLEO-STOCK-TICKER.git`
  (public). Push via SSH (key already authorized as user `Rozakos`).

## 5. Status / milestones

- [x] CubeMX skeleton (ETH MAC, LTDC 480×272 RGB565 FB @0xC0000000, FMC/SDRAM, DMA2D, SDMMC,
      FreeRTOS, FATFS, I2C1/I2C3, USART1).
- [x] Git repo + `.gitignore`/`.gitattributes`, app config/secrets scaffolding, printf console, README.
- [~] **M1 Ethernet** (on-target debugging in progress): LwIP enabled in CubeMX (DHCP, RTOS).
      UART console CONFIRMED WORKING — board boots, LwIP inits, net task runs. DHCP was NOT
      binding; root cause found + fixed (see below). **Awaiting re-test after the link-thread fix.**
- [ ] **M2 HTTPS+JSON**: enable mbedTLS in CubeMX; `https_client.c` (mbedTLS/LwIP sockets) +
      `stock_api.c` + cJSON → print parsed price/change for a symbol.
- [ ] **M3 Display**: vendor LVGL 9.x + Disco BSP; `display.c` (LTDC+DMA2D flush, double buffer in
      SDRAM), `touch_ft5336.c` (LVGL indev on I2C3); add SDRAM region to linker; LVGL tick.
- [ ] **M4 UI**: price/change, sparkline (`lv_chart`), trend screen + range buttons, SD-card logos
      (FATFS). Compile-time symbols from `config.h`; web admin later.
- [ ] Later: web admin (LwIP httpd + settings on SD), WiFi as alternate netif.

## 6. NEXT ACTION (start here)  — for the next agent (Codex)

**State as of last session (verified on hardware):** UART console works. After flash + reset
the board prints:
```
[main] boot OK - USART1 console alive @115200 8N1
[boot] NUCLEO-STOCK-TICKER: LwIP up, starting net task
[net] waiting for link + DHCP...      <-- got stuck here (no DHCP)
```
Ethernet RJ45 green LED blinks (PHY link present). DHCP never bound.

**Root cause (FIXED, needs re-test):** CubeMX generated `LWIP/Target/ethernetif.c` with an
**empty `ethernet_link_thread()`** — it never detected PHY link, never called
`HAL_ETH_Start_IT()`, so the ETH RX path never started and DHCP OFFERs were never received.
Last session filled in the link thread (USER CODE block): reads LAN8742 BMSR (reg 1) link bit,
on link-up reads PSCSR (reg 31) for speed/duplex, calls `HAL_ETH_SetMACConfig` +
`HAL_ETH_Start_IT` + `netif_set_link_up` (and prints `[eth] link UP (100M/full)`); on link-down
stops + `netif_set_link_down`. PHY_ADDRESS=0. The RX `HAL_ETH_Rx*Callback`s are weak HAL
overrides (no registration needed) — already present and correct.

**DO THIS NEXT:**
1. Rebuild + flash + reset. Watch UART. EXPECT now:
   ```
   [eth] link UP (100M/full)
   [net] DHCP bound. IP  = 192.168.x.x
   [net] DNS rozakos.eu -> <ip>
   [net] TCP connect OK (stack reaches the API host)
   ```
2. If `[eth] link UP` prints but DHCP still fails → RX path issue. Check `HAL_ETH_ReadData`
   actually returns frames; verify RX_POOL (`ETH_RX_BUFFER_CNT`) and that pbufs are freed.
   With D-cache OFF the `SCB_InvalidateDCache_by_Addr` in `HAL_ETH_RxLinkCallback` is a no-op
   (fine). Try a static IP (`config.h` NET_USE_DHCP=0 path is NOT wired in net_task yet — would
   need adding) to isolate DHCP vs RX.
3. If `[eth] link UP` never prints → PHY read issue: confirm PHY addr (0), that `HAL_ETH_Init`
   succeeded (else `Error_Handler`), MDIO clock. Consider using ST's `lan8742.c` driver from the
   FW package instead of raw PHY reg reads.
4. Once `TCP connect OK` shows → **M1 DONE**, commit, move to **M2 (mbedTLS HTTPS + cJSON)**:
   enable mbedTLS in CubeMX (raise FreeRTOS heap + netTask stack a lot — TLS is heavy), write
   `app/https_client.c` (mbedTLS over LwIP sockets, `TLS_INSECURE_SKIP_VERIFY` first) +
   `app/stock_api.c` + vendored cJSON; GET `/stocks/api/v1/stock/AAPL` with
   `Authorization: Bearer <STOCK_API_TOKEN>` and print parsed price/change.

### (done) Enabling LwIP in CubeMX — kept for reference

### (done) Enabling LwIP in CubeMX — kept for reference
Open `NUCLEO-STOCK-TICKER.ioc`:
1. **Middleware and Software Packs → LWIP → Enabled**.
2. General: `LWIP_DHCP = Enabled`; ensure `WITH_RTOS`/`NO_SYS=0` (FreeRTOS present);
   enable `LWIP_DNS`, `LWIP_NETCONN`, `LWIP_SOCKET`. Bump `MEM_SIZE` and `PBUF`/`MEMP`
   pools up from defaults.
3. Confirm the ETH PHY = LAN8742, RMII (already set).
4. FreeRTOS: raise `configTOTAL_HEAP_SIZE` (32K → ≥128K once mbedTLS is added) and give the
   LwIP/app tasks generous stacks.
5. Project Manager → keep "Generate peripheral init as pair of .c/.h" and the
   "Keep User Code" option ON. **Generate Code.**
6. Verify CubeMX added: `LWIP/App/lwip.c`, `LWIP/Target/ethernetif.c`, `lwipopts.h`, linker
   sections (`.RxDecripSection`/`.TxDecripSection`/`.lwip_sec`), and MPU config in `main.c`.
   These must build clean before app code is added.

After regen, an agent writes `Core/Src/app/net_task.c` (FreeRTOS task: wait for
`netif_is_up`+DHCP bound, `dns_gethostbyname("rozakos.eu")`, raw TCP connect to test),
starts it from `freertos.c`/`main.c` USER CODE, and verifies over UART.

## 7. Risks / gotchas

- **D-cache/MPU coherency** for ETH DMA descriptors/Rx pool and the SDRAM framebuffer — the
  single most likely source of subtle bugs. Use CubeMX's generated MPU config / the example's.
- mbedTLS handshake is **RAM/stack/time-heavy** on F746 (~1–3 s, deep stack). Size task stacks
  generously; keep big buffers in SDRAM where possible.
- Don't commit `secrets.h`. Don't edit generated code outside USER CODE blocks.
- Many Disco peripherals are enabled but unused (DCMI/SAI/SPDIF/QSPI/USB host) — ignore them.

## 8. Session log

- **2026-06-09 — Claude (Opus 4.8, Claude Code):** git init + ignore/attributes; `Core/Inc/app/`
  `config.h` + `secrets.h(.example)`; printf→USART1 retarget in `main.c`; `README.md`; this
  `AGENTS.md`. Decided CubeMX route for LwIP/mbedTLS. Pushed to GitHub.
- **2026-06-09 — Claude (Opus 4.8, Claude Code):** on-target M1 debugging. Fixed build error
  (`MEMP_NUM_SYS_TIMEOUT` 5→8 in `lwipopts.h` USER CODE, needed once DNS on). Added a raw-UART
  boot diagnostic in `main.c` USER CODE 2 (confirmed console works). Diagnosed DHCP-not-binding:
  CubeMX left `ethernet_link_thread()` EMPTY in `LWIP/Target/ethernetif.c`; implemented PHY
  link detection + `HAL_ETH_Start_IT` + `netif_set_link_up` (LAN8742, addr 0) in its USER CODE
  block, plus PHY defines + `<stdio.h>` in USER CODE 0. **Handed off to Codex for re-test — see §6.**
- **2026-06-09 — Claude (Opus 4.8, Claude Code):** user enabled LwIP in CubeMX + regenerated
  (ETH ownership moved to `ethernetif.c`, `MX_LWIP_Init` in StartDefaultTask). Verified caches
  are OFF (no ETH-DMA coherency issue yet). M1 code: enabled `LWIP_DNS` + `MEM_SIZE 16K` in
  `lwipopts.h` USER CODE; added `.lwip_sec` RAM section + heap bump in `STM32F746NGHX_FLASH.ld`;
  wrote `app/net_task.{c,h}` (DHCP wait → DNS → TCP connect, logged on USART1); created the task
  in `main.c`. Token set locally in `secrets.h` (git-ignored). **Next = on-target M1 verify (§6).**
