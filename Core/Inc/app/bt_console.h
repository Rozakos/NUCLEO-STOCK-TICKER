/**
 * bt_console.h  —  Bluetooth SPP admin console and price alerts.
 *
 * Pair with the HC-05 from a phone (any SPP terminal app), and this gives the
 * same control the web admin does — watchlist, shares, refresh interval —
 * plus per-symbol price alerts pushed out as they trigger. Unlike the web
 * admin it needs no network at all, which is exactly when you tend to want it.
 */
#ifndef APP_BT_CONSOLE_H
#define APP_BT_CONSOLE_H

#include "cmsis_os.h"

/** Bring up the HC-05 and start the console/alert task. */
void bt_console_start(void);

/** Task entry point (exposed for main.c's task table). */
void StartBtConsoleTask(void const *argument);

#endif /* APP_BT_CONSOLE_H */
