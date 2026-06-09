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
- [x] **M1 Ethernet**: verified on hardware. Link up, DHCP bound (`192.168.1.154`), DNS resolved
      `rozakos.eu`, and repeated TCP connects to port 443 succeeded.
- [x] **M2 HTTPS+JSON**: vendored STM32Cube F7 mbedTLS 2.16.2 and cJSON. HTTPS stock API client
      uses hardware RNG, bearer auth, SNI, and an SDRAM TLS allocation arena. Verified on target:
      AMD quote and sparkline data fetch successfully from `rozakos.eu`.
- [x] **M3 Display**: SDRAM/LTDC/framebuffer + LVGL 9.5.0 + FT5336 touch verified on hardware.
      Now **double-buffered** (two SDRAM framebuffers FB0=0xC0000000, FB1=0xC0080000; LVGL DIRECT
      mode renders into the back buffer, `display_flush` swaps the LTDC address during vblank) —
      fixes the full-screen blink on scroll / detail screen. (Optional later: LVGL DMA2D draw unit
      for render accel.)
- [~] **M4 UI**: live quote snapshots update symbol, price, percentage, HTTPS status, and
      sparkline. Now **multi-symbol** (default AMD, NVDA, AAPL, MSFT, TSLA; configurable via web
      admin up to APP_MAX_SYMBOLS=8). Tapping any row opens a detail screen (AMD shows its logo;
      others show a brand-colored initial badge) with a full-width chart and range buttons that
      asynchronously fetch real `/history/{symbol}?range=...` data via the serialized TLS task.
- [ ] Later: web admin (LwIP httpd + settings on SD), WiFi as alternate netif.

## 6. NEXT ACTION (start here)

**M1 is verified complete.** On-target output:
```
[eth] link UP (100M/full)
[net] DHCP bound. IP  = 192.168.1.154
[net] DNS rozakos.eu -> 172.67.155.109
[net] TCP connect OK (stack reaches the API host)
```

**M1 root cause (FIXED and verified):** CubeMX generated no project
`HAL_ETH_MspInit()` override. `HAL_ETH_Init()` therefore called the HAL weak no-op, leaving
ETH clocks, RMII GPIO alternate functions, and the ETH IRQ unconfigured. `ethernetif.c` now
implements the STM32F746G-DISCO RMII MSP setup inside USER CODE 3 and calls
`HAL_ETH_SetMDIOClockRange()` after HAL init. PHY reads now report MDIO failures explicitly
instead of treating every failure as link-down.

**DO THIS NEXT:**
1. Tap the AMD row and each range button. Confirm the chart and period percentage change after
   the `Loading <range> history...` state. Expected UART:
   ```
   [touch] press x=<x> y=<y>
   [ui] row clicked: AMD
   [ui] history range requested: 1mo
   [history] fetching AMD 1mo...
   [history] AMD 1mo: <n> points
   ```
2. Web admin is verified at `http://192.168.1.154/` on the current DHCP lease. The current
   address is always printed at boot as `[web] admin ready: http://<ip>/`.
3. Next UI work: add chart axis/date labels and use `session_open`/`session_close` for a
   progressive intraday chart.

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

- **2026-06-09 — Claude (Opus 4.8, Claude Code):** Added company logos from the API (NOT yet
  on-target tested). API (github.com/Rozakos/stock-api) serves logos only at GET /logo/{symbol}
  ?size=32|48|64 as PNG (no logo field in the quote JSON; no raw RGB565 option). Chosen path:
  decode PNG on-device. Enabled `LV_USE_LODEPNG`, moved LVGL heap+cache to SDRAM (LV_MEM_ADR
  0xC0100000, 2 MB; LV_CACHE_DEF_SIZE 512 KB) so decode has room and logos aren't re-decoded each
  frame. New `app/logo_cache.{c,h}` (per-symbol PNG slots in SDRAM @0xC0300000, thread-safe).
  `stock_api`: `https_get` now returns body length (binary-safe) + `stock_api_fetch_logo`.
  `net_task` fetches each symbol's logo once. `ui_task` shows the cached logo on rows (swapped in
  by `update_rows` when ready) and the detail screen; colored-initial badge remains the fallback.
  Reuses bundled AMD asset as AMD's immediate fallback. SDRAM map documented in lv_conf.h /
  logo_cache.c. **NEXT: flash + verify `[logo] <sym> cached` lines and logos rendering.**
- **2026-06-09 — Claude (Opus 4.8, Claude Code):** Fixed on-device blink + detail-screen layout.
  Root cause of blink (both market-list scroll and detail screen): LVGL rendered in DIRECT mode
  into the single live LTDC framebuffer. Switched to **double buffering** (FB0/FB1 in SDRAM;
  `display_flush` now swaps the LTDC base address during vblank via `HAL_LTDC_SetAddress`, timeout-
  guarded vsync poll on `LTDC_CDSR_VSYNCS`). Detail screen overflowed 480x272 (range row aligned
  BOTTOM_MID +22 = below the edge → forced scrolling); moved range row to -28 and status to -6,
  reduced chart height 142→128, disabled detail-screen scrolling. Reviewed app modules for bugs —
  no other real bugs found (TLS is single-fetcher so shared SDRAM buffers are safe; VERIFY_NONE is
  the intended bring-up shortcut, CA pinning is a later item).
- **2026-06-09 — Claude (Opus 4.8, Claude Code):** Wired all symbols into the UI (was AMD-only).
  The UI/net/web already handled multiple symbols — only the default list was AMD-only and the
  detail screen hard-coded `logo_AMD`. Set `config.h` default to AMD/NVDA/AAPL/MSFT/TSLA (5), and
  made `create_detail_screen` show a brand-colored initial badge for non-AMD (AMD keeps its logo),
  mirroring `create_badge`. Verified no other AMD hardcodes remain. **Not yet on-target verified.**
- **2026-06-09 — Claude (Opus 4.8, Claude Code):** Reviewed Codex's uncommitted web-admin work
  (runtime symbol add/delete + refresh-interval via POST forms; `receive_request`/`form_value`
  HTTP parsing; multi-symbol rotation in `net_task`). Found + fixed a **webTask stack overflow**:
  the enlarged `request[1536]` + `form[2300]` buffers (~4.1 KB) exceeded the 1024-word (4 KB)
  stack → bumped `webTask` to 2048 words in `main.c`. Verified no divide-by-zero (symbol_count
  ≥1 invariant holds), `<ctype.h>` present, critical-section use consistent. Committed + pushed.
- **2026-06-09 — Codex (GPT-5):** Wired detail range controls to real history API requests.
  Added a thread-safe generation-based history request/result mailbox so LVGL never blocks on
  HTTPS and stale responses cannot overwrite newer taps. The network task wakes immediately,
  fetches `/history/{symbol}?range=...&limit=64`, parses `interval` and `{ts,last}` points, and
  updates the detail chart and period percentage. Live API contract validated, Debug build
  linked and flashed, and Web UI verified with HTTP 200 at `http://192.168.1.154/`.
- **2026-06-09 — Codex (GPT-5):** Implemented list-to-detail navigation for the AMD ticker.
  Tapping the row now opens a dedicated LVGL screen with the bundled AMD logo, live price/change,
  a full-width chart based on the current quote sparkline, Back navigation, and visible
  `1D/1W/1M/6M/1Y/5Y/Max` range controls. Range selection updates the screen and UART; fetching
  distinct history datasets remains the next API task. Debug build linked successfully.
- **2026-06-09 — Codex (GPT-5):** Ported the original bundled AMD 48×48 ARGB8888
  LVGL asset without adding a PNG decoder. AMD rows now show the native logo, visibly
  highlight while pressed, and report clicks in both the status bar and UART. FT5336 input
  now logs one transformed coordinate line per new press. Debug build linked successfully;
  next on-target check is to confirm the logo renders and compare `[touch] press x=... y=...`
  with `[ui] row clicked: AMD`.
- **2026-06-09 — Codex (GPT-5):** Fixed live HTTPS data fetching and switched configuration to
  AMD-only. COM4 showed Cloudflare fatal TLS alerts because SNI was compiled out; enabling SNI
  exposed a certificate-chain curve parse error, fixed by enabling P-384/SHA-384 support.
  HTTPS then succeeded. Added embedded-safe fixed-point formatting because newlib-nano omits
  `%f` support. Rebuilt, flashed, and verified on COM4:
  `[stock] AMD 490.33 (+5.14%), 5 spark points`.
- **2026-06-09 — Codex (GPT-5):** Started the full functional port after LVGL verification.
  Read the original CYD API behavior; vendored mbedTLS 2.16.2 and cJSON; implemented hardware-RNG
  TLS, HTTP/1.0 bearer-auth quote fetching, cJSON parsing, thread-safe stock snapshots, live LVGL
  price/change/sparkline updates, and direct FT5336 touch input on I2C3. TLS heap and HTTP buffer
  live in SDRAM beyond the framebuffer. Combined build linked at about 827 KiB and was flashed.
  COM4 remains occupied by MobaXterm, so live quote/touch confirmation is pending.
- **2026-06-09 — Codex (GPT-5):** User visually confirmed the stable eight-color-bar test,
  proving SDRAM initialization, LTDC, panel timing, and the RGB565 framebuffer work. Vendored
  LVGL 9.5.0, added a direct-framebuffer display driver and first stock-ticker placeholder UI,
  increased the FreeRTOS heap to 128 KiB, built successfully, and flashed the board. Awaiting
  visual confirmation of the LVGL screen.
- **2026-06-09 — Codex (GPT-5):** User confirmed the M1 MSP fix fully works: link, DHCP, DNS,
  and repeated TCP connections to `rozakos.eu:443` succeeded. Switched to graphics. Confirmed
  CubeMX LTDC timings match the RK043FN48H BSP, found the mandatory SDRAM device initialization
  sequence was missing, and added `app/display.{c,h}` to initialize/verify SDRAM and draw an
  RGB565 color-bar test pattern. Clean Debug build passed with zero warnings and was flashed.
  Awaiting visual/UART confirmation before adding LVGL.
- **2026-06-09 — Codex (GPT-5):** Continued M1 on-target debugging after UART reported
  `[eth] link DOWN`. Found CubeMX had omitted `HAL_ETH_MspInit()` entirely, so the HAL weak
  no-op left ETH clocks/RMII GPIOs/IRQ unconfigured. Added STM32F746G-DISCO RMII MSP setup in
  `ethernetif.c` USER CODE, configured the MDIO clock divider, and added explicit PHY-read error
  logging. Debug build passed; DMA descriptors confirmed in RAM; firmware flashed successfully.
  COM4 capture was blocked by the user's open MobaXterm session; next step is inspect its output.
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
