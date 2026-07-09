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
  Endpoints: `GET /stocks?symbols=A,B,C` (batch quotes), `GET /stock/{symbol}`,
  `GET /history/{symbol}?range=1d|1w|1mo|6mo|1y|5y|max`, `GET /logo/{symbol}?size=48`.
  Quotes carry extended-hours fields: `market_state` (PRE/REGULAR/POST/CLOSED),
  `pre_market`/`pre_market_change_pct` (non-null only during PRE) and
  `post_market`/`post_market_change_pct` (POST/CLOSED); change % is vs regular close.
  History `range=1d` supports `prepost=1` (deployed API-side 2026-07-08): points span
  the extended 04:00–20:00 ET window and the response adds `window_open`/`window_close`
  (fixed axis bounds), `market_state` and `prev_close`; `session_open`/`session_close`
  keep meaning the regular 09:30/16:00 bounds. The flag is ignored for crypto and
  non-1d ranges, and omitting it returns the old byte-identical response. Crypto
  (`-USD` suffix, CoinGecko-backed) works with no firmware change — symbol validation
  already accepts `-`.

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
- [x] **Extended hours + night theme (2026-07-08)**: snapshots carry
      `ext_state/ext_price/ext_change_pct`; during PRE/POST the rows, detail header and
      portfolio total follow the extended print, the status bar shows PREMARKET/AFTER HOURS
      with a drawn crescent moon, and the market screen switches to a purple night palette
      (`apply_market_theme` in ui_task.c). Verified live during after-hours.
      Extended same day: the title is now driven by the API's `market_state` (snapshot
      `session` field) through all four states — PREMARKET / MARKET OPEN (amber sun =
      bare disc, day palette) / AFTER HOURS / MARKET CLOSED (moon, night palette) —
      and the right status-bar slot shows portfolio total + day P/L vs prev close
      (green/red, ext-aware) instead of the old uptime counter, with an amber
      "stale Ns" takeover when no refresh lands for 2 intervals + 5 s.
      Verified live during the open session (`[ui] session: MARKET OPEN`).
- [x] **Settings persistence without SD (2026-07-08)**: flash sector 7 blob
      (`0x080C0000`, magic+checksum, skip-if-unchanged) as fallback when SD is absent;
      linker FLASH capped at 768K to reserve the sector. Verified end-to-end on target
      (web-UI add → reboot → loaded from flash). Sector erase stalls the CPU ~1 s.
- [x] **Tear-free page flip (2026-07-08)**: `display_flush` writes the layer shadow
      CFBAR + VBR reload (hardware applies in vblank) instead of polling the ~0.6 ms
      VSYNC status bit — the old poll missed it and fell back to an immediate mid-scanout
      reload, the user-visible "half screen" glitch.
- [~] **M4 UI**: live quote snapshots update symbol, price, percentage, HTTPS status, and
      sparkline. Now **multi-symbol** (default AMD, NVDA, AAPL, MSFT, TSLA; configurable via web
      admin up to APP_MAX_SYMBOLS=8). Tapping any row opens a detail screen (AMD shows its logo;
      others show a brand-colored initial badge) with a full-width chart and range buttons that
      asynchronously fetch real `/history/{symbol}?range=...` data via the serialized TLS task.
      Detail screen is now a full CYD `detail_screen.cpp` port: `lv_chart` + grid, spinner
      while loading, gradient area fill under the curve, snapped price ticks on Y, date/time
      ticks on X, monotone-cubic smoothing, last-point marker dot, instant range-button
      highlight (pending vs displayed split), and a progressive 1D session chart
      (`session_open`/`session_close` parsed from the API). Flashed 2026-06-11; awaiting
      visual confirmation.
- [x] **Web admin**: runtime symbol add/delete, shares and refresh interval work at
      `http://<board-ip>/`. Settings persist through `ticker.cfg` on SD when a formatted
      card is available, else the flash sector-7 blob (this board has no SD card).
- [x] **TLS verification**: `rozakos.eu` hostname and certificate chain are verified against
      the pinned Google Trust Services WE1 intermediate (valid through 2029-02-20).
- [x] **Extended-hours 1D chart + session views (2026-07-09)**: 1d history fetched with
      `prepost=1`; snapshot carries `window_open`/`window_close`/`prev_close`. The
      progressive 1D axis spans the extended window, faint dividers mark the regular
      open/close, middle X ticks snap to them, and re-tapping the active 1D button opens
      a session-view dropdown (Full day / Pre-market / Live market / After hours), gated
      by which segments have data. Verified on target during pre-market; market/after
      states pending a live session.
- [ ] Later: WiFi as alternate netif.

## 6. NEXT ACTION (start here)

Everything through the extended-hours 1D chart is flashed; pre-market state verified
on target (2026-07-09).

**DO THIS NEXT:**
1. **Verify the 1D session-view dropdown through a full trading day** — it shipped
   during pre-market, when the dropdown is intentionally inert. During the live session
   (16:30–23:00 Greece time) re-tapping the active 1D button should offer Full day /
   Pre-market / Live market; after close, After hours joins. Optional polish, explicitly
   NOT done (the dropdown replaced it per user direction): drawing the pre/post segments
   of the Full-day view in the amber accent (0xFBBF24).
2. WiFi as an alternate netif (the status bar already reserves a dimmed WiFi icon).
3. Debugging a hang/crash? See the device-debug workflow: serial on COM4, web-UI POSTs
   via `Invoke-WebRequest -UseBasicParsing`, and hot-attach forensics with
   `STM32_Programmer_CLI -c port=SWD mode=HOTPLUG -r32/-coreReg` +
   `arm-none-eabi-addr2line`. A FreeRTOS stack overflow presents as total silent death
   (the hook's printf is swallowed by the UART HAL lock).

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

- **2026-07-09 — Claude (Fable 5, Claude Code):** Adapted to the API's `prepost=1` 1d
  history (deployed API-side 2026-07-08). `stock_api_fetch_history` appends `prepost=1`
  for the 1d range and parses `window_open`/`window_close`/`prev_close` into the
  history snapshot (whole-struct mailbox — no plumbing changes needed). In
  `render_history`: the progressive 1D axis now prefers the extended window (fallbacks:
  session bounds, then the 6.5 h heuristic), faint vertical dividers mark the regular
  open/close (drawn with the gradient in `chart_area_fill`), the two middle X ticks
  snap to the dividers (user feedback: even spacing put "16:20" beside the 16:30 open
  and read as wrong), and the 1D day-change baseline prefers the API's `prev_close`
  over the live-quote derivation. Added the 1D **session-view dropdown** (user request,
  superseding the planned amber pre/post coloring): re-tapping the active 1D button
  opens a modal menu — Full day / Pre-market / Live market / After hours — gated by
  data presence (inert during pre-market; + Live market during the session; all four
  after the close). Segment views trim points and axis to the segment and re-render
  from the cached snapshot with no refetch; anything missing bounds (older API, crypto)
  or with <2 points in the segment falls back to Full day. Menu deletes async (closes
  from its own event callbacks); state resets with the detail screen. Also confirmed
  the API's crypto addition needs no firmware change (`valid_symbol_char` already
  accepts `-`). Verified on target during pre-market: clean boot, PREMARKET session,
  quotes live, dropdown correctly inert; live/after-hours states await the session.

- **2026-07-08 (later) — Claude (Fable 5, Claude Code):** Status bar made useful. The
  center title was a static "MARKETS" leftover; now `parse_extended_hours()` also reads
  the API's `market_state` string into a new `stock_session_t session` snapshot field
  (fallback: infer PRE/POST from the extended price fields for older API deploys) and
  `apply_market_theme()` keys off it: PREMARKET / MARKET OPEN / AFTER HOURS / MARKET
  CLOSED, amber sun disc while open (the crescent's mask hidden = full disc), moon +
  night palette for everything else (CLOSED is now night too — was day "MARKETS").
  The right slot's "updated Ns" was **uptime**, not update age — replaced with
  portfolio total + day P/L (`$12345 ▲ 1.23%`, holdings at ext-aware price vs prev
  close backed out of `last/(1+change_pct/100)`), green/red; empty without shares;
  amber `stale Ns` takeover when the newest snapshot is older than 2 refresh intervals
  + 5 s. Verified on target mid-session: `[ui] session: MARKET OPEN`, and the user's
  web-UI watchlist survived the reflash via the sector-7 store.

- **2026-07-08 — Claude (Fable 5, Claude Code):** Extended-hours support + three fixes,
  all verified on target during live after-hours. (1) **Pre/after-market quotes**:
  API now nulls `pre_market`/`post_market` outside their session, so
  `parse_extended_hours()` (both quote paths) keys off numeric presence; snapshots
  gained `ext_state/ext_price/ext_change_pct`. Rows/detail/portfolio follow the
  extended print; status bar retitles PREMARKET/AFTER HOURS with a two-disc crescent
  moon; market screen swaps to a purple night palette on session transitions only.
  (2) **1D chart colored by day change**: was first→last of the window (green on a
  gap-down day that climbed off the open); now anchored to prev close derived from the
  live quote (`last/(1+pct/100)`) — history API carries no prev_close (requested,
  with `prepost=1`, from the API side). (3) **Graph-tab "not refreshing"**:
  `render_history` stomped the header price back to the session close on every silent
  refresh; header now prefers the live extended-aware quote. (4) **Half-screen glitch**
  = real tearing: vsync poll (1 ms granularity vs ~0.6 ms pulse) missed → timeout →
  immediate mid-scanout reload; replaced with CFBAR shadow write + VBR reload (hardware
  swaps in vblank). (5) **Settings persistence without SD**: board has no card
  (`[settings] SD unavailable` meant `storage_ready` stayed false and saves no-op'd);
  added a flash sector-7 blob store (magic+checksum, skip-if-unchanged, D-cache
  invalidate around program), SD → flash → defaults load order, linker FLASH capped at
  768K. First on-target save **hard-hung the board**: hot-attach forensics (PC in
  `vApplicationStackOverflowHook`, `pxCurrentTCB->pcTaskName` = "webTask") showed the
  save frames overflowed the 8 KB web stack — and the overflow hook prints nothing
  because the interrupted printf holds the UART HAL lock. webTask 2048→4096 words,
  uiTask 2048→3072 (update_rows→update_detail nests two ~2.8 KB snapshot arrays),
  save blob made static. Round-trip verified: add symbol → `saved to flash` → reboot →
  `loaded from flash` → delete → clean save, device alive throughout. Also: ethernet
  status icon is now a drawn RJ45 jack (no glyph in the LVGL symbol font) + dimmed
  WiFi placeholder; `stock_data_get_symbol()` accessor added.

- **2026-06-12 — Claude (Fable 5, Claude Code):** Adopted the API's new **batch quote
  endpoint** (`GET /stocks?symbols=A,B,C` -> `{"quotes":[{symbol,last,change_pct,closes}]}`,
  deployed on rozakos.eu and probed live). New `stock_api_fetch_quotes()`; `net_task` now does
  ONE quote request per refresh cycle (was one per symbol per `refresh/count` slice), then
  pending logos (preempted by history requests), then sleeps the full refresh interval.
  Symbols missing from the batch response get a "no quote" status snapshot. Verified on
  target: boot handshake 2.4 s, then 5 quotes via one request + 4 logos on the same
  connection; history taps (1d/1mo) fetched with zero handshakes, near-instant.

- **2026-06-12 — Claude (Fable 5, Claude Code):** Snappiness part 2 (all firmware, no API
  changes): (1) **Persistent HTTPS connection** — `stock_api.c` rewritten around one
  long-lived TLS connection (HTTP/1.1 `Connection: keep-alive`); probed the live API first
  (curl): Cloudflare always returns `Content-Length` for identity encoding, so the reader is
  CL-based (chunked = error, read-to-close fallback marks the connection dead). Transport
  failure on a REUSED connection auto-reconnects + retries once; HTTP errors are final.
  (2) **TLS session resumption** — `MBEDTLS_SSL_SESSION_TICKETS` enabled,
  `mbedtls_ssl_get/set_session` around each reconnect, so post-idle reconnects skip the key
  exchange. (3) **D-cache enabled** — mirrored ST's LwIP example MPU layout: region 1
  = 0x20048000 16 KB normal non-cacheable (LwIP heap via `LWIP_RAM_HEAP_POINTER`, ETH DMA
  reads TX pbufs from it; MEM_SIZE capped 16K→15K so heap+overhead stays under 0x2004C000);
  region 2 = 0x2004C000 1 KB device (ETH descriptors — `.lwip_sec` now fixed there via new
  ETHRAM linker region; RAM region shrunk to 288 K; `_estack` pinned at 0x20050000, MSP grows
  down over SRAM2 like ST's example). RX_POOL stays cacheable (template already does
  `SCB_InvalidateDCache_by_Addr` per buffer). SD diskio cache maintenance + scratch buffer
  defines enabled. **Verified on target**: descriptors at 0x2004C000 (map), DHCP, quotes,
  logos, history, UI all working; boot handshake 2.3 s, then ZERO handshakes — every request
  reuses the connection (quote cycle + logos ride one socket). Note: D-cache gain on TLS is
  modest because the mbedTLS arena lives in non-cacheable SDRAM (0xC0040000) by design.

- **2026-06-12 — Claude (Fable 5, Claude Code):** Performance: discovered the M7 ran with
  **both CPU caches OFF** and the Debug build at **-O0**. Added `[tls] handshake Nms` timing
  to `https_get`, measured baseline **7.7-10.7 s per TLS handshake**. Enabled `SCB_EnableICache()`
  (USER CODE 1; I-cache has no DMA-coherency hazard — D-cache deliberately still OFF pending
  ETH descriptor MPU work) and switched the Debug config to **-O2** (.cproject + regenerated-
  makefile sed; IDE re-syncs on next build). Result, verified on COM4: handshakes now
  **2.4-2.9 s (~3.7x)**, text shrank 970->686 KB, and the user exercised ALL ranges
  (1d/1w/1mo/6mo/1y/5y/max) — every one fetched + rendered, no freeze. Next snappiness steps
  (not done): TLS session resumption (mbedtls_ssl_get/set_session), persistent HTTP/1.1
  keep-alive connection, D-cache + MPU, API batch-quote endpoint
  (github.com/Rozakos/stock-api has no multi-symbol quote endpoint yet).

- **2026-06-12 — Claude (Fable 5, Claude Code):** Market-list polish from user feedback:
  (1) list no longer pans — rows are positioned manually (no flex) and the list is
  non-scrollable; every count stretches row heights to fill the 244 px exactly (≤4 one
  full-width column; 5-8 two balanced columns, ceil/floor split, the shorter column gets
  taller rows — no dead space for 5 or 7). (2) AMD always uses the bundled white/green
  `logo_AMD` asset (API PNG is near-black, invisible on the dark theme); net_task skips the
  AMD logo fetch. (3) Compact rows stack icon (24 px) above the name, freeing width for an
  84 px sparkline; all rows (both layouts) now draw a gradient fill under the sparkline via
  `chart_util_draw_polyline_fill` on `LV_EVENT_DRAW_MAIN_BEGIN` (per-row point/color state in
  `market_row_t`). Badges are now LEFT_MID-aligned (were top-left, visibly off in tall rows).
  **1M freeze report**: could NOT reproduce after the priority/yield fix — `AMD 1mo: 22
  points` and `6mo: 64 points` fetched + rendered fine on COM4. One unexplained mid-session
  reboot was observed once (no fault print; possibly the user's reset press) — if freezes
  recur, suspect a HardFault and add a fault-handler UART dump first.

- **2026-06-11 — Claude (Fable 5, Claude Code):** Three UX features (flashed, pending visual
  confirm): (1) **Adaptive market list** — >4 symbols switches the list to two side-by-side
  columns of compact rows (`LV_FLEX_FLOW_COLUMN_WRAP`, 4 per column, no sparkline, smaller
  fonts/badges) so up to 8 symbols never scroll; ≤4 keeps full-width rows. (2) **Shares
  owned** — new `settings_get/set_shares*` (index-aligned with symbols, persisted as a
  `shares=` CSV line in `ticker.cfg`), web UI per-symbol qty input (`POST /shares`; NOTE:
  `form_value()` truncates at '&', so extract the LAST form field first), holdings value on
  the web Live Markets cards, "12.50 sh = $5916.25" in the detail header (tracks live quote),
  and portfolio total in the status bar ("$12345 | 42s"). (3) **Network info popup** — tapping
  the status-bar wifi icon opens a modal (top layer, tap-anywhere closes) with link state,
  IP/GW/mask/DNS (`_r` ntoa variants — plain `ip4addr_ntoa` shares one static buffer), MAC,
  web-admin URL, uptime; icon turns red on link/lease loss. Capacity question answered: SDRAM
  is nowhere near a limit (~4.9 MB free; logos ≈25 KB/symbol incl. decode); going to 16
  symbols needs UI task stack bump (by-value `stocks[APP_MAX_SYMBOLS]` copies), a grid/paged
  list layout, and slower refresh or a batch quote endpoint (TLS handshake ≈1-3 s each).

- **2026-06-11 — Claude (Fable 5, Claude Code):** Fixed TLS failures during history requests
  (`-0x004E` = `MBEDTLS_ERR_NET_SEND_FAILED`). Root cause: `uiTask` ran at AboveNormal — above
  the LwIP `tcpip_thread` and `netTask` (both Normal) — and the new detail-screen spinner keeps
  LVGL rendering continuously while `display_flush` busy-waited up to 20 ms per frame for
  vsync. The starved TCP stack stopped ACKing mid-handshake and Cloudflare reset the
  connection. Fix: `uiTask` → Normal (FreeRTOS time-slicing shares fairly) and the vsync wait
  now yields (`osDelay(1)` in the poll loop). Also made failed logo fetches retry (up to 3
  attempts; previously one transient failure blanked the logo until reboot). **Verified live
  on COM4**: history 1d/1w/1y fetched and rendered while the spinner animated and quote/logo
  fetches ran concurrently; rapid 1mo→1y taps correctly superseded the stale request; zero
  TLS errors. UI remains smooth at Normal priority.

- **2026-06-11 — Claude (Fable 5, Claude Code):** Ported the CYD detail screen properly
  (user: screen "doesn't update nicely, no round progress bar, no gradients, no axis labels").
  Fetched `src/ui/detail_screen.cpp` + `src/util/{area_fill,interpolate}.cpp` from the CYD repo
  and ported to C/LVGL 9.5: new `app/chart_util.{c,h}` (Fritsch-Carlson monotone cubic
  smoothing, row-rasterized gradient polyline fill, epoch→civil date conversion); detail
  screen rebuilt around `lv_chart` (was `lv_line`) with grid lines, `lv_spinner` during
  fetches, alpha-gradient area fill via `LV_EVENT_DRAW_MAIN_BEGIN`, snapped Y price ticks in
  a measured gutter, 4 X date/time ticks (format auto-picks HH:MM / DD Mon / Mon YY / YYYY by
  window span), last-point marker dot, pending-vs-displayed range state (instant tap
  highlight, error revert + retry), and the progressive 1D session chart — `stock_api.c` now
  parses `session_open`/`session_close` (fields confirmed in the live API and CYD's
  `quote_fetcher.cpp`). Added `APP_UTC_OFFSET_MINUTES` (config.h, +180 EEST) for axis times.
  Spinner/arc/chart widgets were already compiled in (lv_conf defaults). Built via bundled
  make + GNU tools (headless CubeIDE blocked: IDE workspace open; hand-added chart_util to
  git-ignored `Debug/.../subdir.mk` + `objects.list` — IDE regenerates these), zero warnings,
  text 970 KB. Flashed + MCU reset OK. **Awaiting on-screen visual confirmation (§6).**

- **2026-06-09 - Codex (GPT-5):** Restored fetched PNG logo decoding by fixing the actual
  external-LVGL-heap root cause. Cortex-M7's default map treated `0xC0000000` SDRAM as Device
  memory, where LVGL TLSF's normal/unaligned accesses stalled. Added an 8 MiB normal
  non-cacheable MPU region before HAL initialization, then re-enabled the 2 MiB SDRAM LVGL
  heap, 512 KiB image cache, and lodepng. Clean-built, flashed, and verified full UI startup,
  pinned-TLS quote fetches, and `[ui] logo applied: AAPL` after decoding a fetched PNG.

- **2026-06-09 - Codex (GPT-5):** Enabled required TLS certificate verification. Inspected the
  live `rozakos.eu` chain, pinned the Google Trust Services WE1 intermediate (valid through
  2029-02-20), enabled PEM/base64 parsing, and changed the default from `VERIFY_NONE` to
  `VERIFY_REQUIRED` with hostname checking. Built, flashed, and verified on target:
  `[tls] pinned CA loaded; verification required`, followed by successful live quote and
  history HTTPS requests.
- **2026-06-09 - Codex (GPT-5):** Added persistent Web UI settings using the existing
  reentrant FatFs/SD stack. The Web task mounts SD, loads `ticker.cfg`, and successful symbol
  or refresh changes atomically replace the config through `ticker.tmp`. Missing/unformatted
  SD cards safely use compile-time defaults. Built, flashed, and verified the fallback path on
  target; hardware reported SD unavailable, so persistence with a card remains to be confirmed.
- **2026-06-09 - Codex (GPT-5):** Restored stable startup after the logo commit caused
  `lv_display_create()` to hang on the first allocation from an external-SDRAM LVGL heap.
  Returned LVGL to its proven internal 64 KiB heap and disabled lodepng until the SDRAM
  allocator path is reliable. Reduced the idle default-task stack, starts the UI before the
  network task, checks app task creation, and added fatal FreeRTOS malloc/stack diagnostics.
  Fixed history requests accidentally being serviced only before DHCP. Clean-built, flashed,
  and verified UI/touch startup, link/DHCP, Web UI HTTP 200, Web UI symbol add, and live
  AMD/NVDA/AAPL quote cycles on target.

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
