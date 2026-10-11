#ifndef APP_WATCHDOG_H
#define APP_WATCHDOG_H

#include <stddef.h>
#include <stdint.h>

/* Hardware watchdog (IWDG, ~16 s) fed by a supervisor that only feeds it
 * while every registered task loop is still checking in. A hung TLS read,
 * a deadlock or a dead scheduler therefore resets the board instead of
 * freezing it until someone presses reset. */
typedef enum
{
  WATCHDOG_UI = 0,   /* LVGL loop: checks in every frame */
  WATCHDOG_NET,      /* quote/history loop: checks in at least every ~30 s */
  WATCHDOG_CLIENT_COUNT
} watchdog_client_t;

/* Print why the board last reset (power-on, button, watchdog, crash) and
 * any crash record left by the fault handler. Call once early at boot. */
void watchdog_report_reset_cause(void);
/* Start the IWDG; it cannot be stopped again until the next reset. */
void watchdog_start(void);
void watchdog_checkin(watchdog_client_t client);
/* Call every ~0.5 s from the supervisor (default task). */
void watchdog_service(void);
/* "Power-on", "Watchdog: NET loop stalled", "Crash: HardFault at 0x...". */
const char *watchdog_last_reset_text(void);

/* Called by the fault handlers / FreeRTOS hooks (fault.c). Records the
 * crash in no-init RAM, dumps it to the console UART without the HAL (its
 * lock may be held), then resets. Never returns. */
void fault_record_and_reset(const char *kind, uint32_t pc, uint32_t lr,
                            const char *detail);

#endif /* APP_WATCHDOG_H */
