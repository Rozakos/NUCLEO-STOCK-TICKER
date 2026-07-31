/**
 * hc05.h  —  HC-05 Bluetooth SPP module on UART7.
 *
 * The HC-05 is a Bluetooth 2.0 Serial Port Profile bridge: a wireless serial
 * cable, not a network interface. It carries no IP, so unlike the ESP-01 it
 * can never be a net_link — it is used for talking *to* the board (admin
 * console, log mirror, price alerts), not for reaching the internet.
 *
 * Wiring (STM32F746G-DISCO Arduino analog header, see AGENTS.md §9):
 *   HC-05 RXD <- PF7 / Arduino A4 (UART7_TX)   [3.3 V logic]
 *   HC-05 TXD -> PF6 / Arduino A5 (UART7_RX)
 * UART7 is configured entirely here (clock, GPIO AF8, NVIC) rather than in
 * the .ioc, so no CubeMX regeneration is needed; PF6/PF7 are declared as
 * unused ADC3 inputs there and nothing in the app touches ADC3.
 *
 * Transmission never blocks: bytes go into a ring drained by the TXE
 * interrupt, and a full ring drops rather than stalls. printf() is mirrored
 * here from every task, and blocking on a 9600-baud link would have been a
 * system-wide brake.
 */
#ifndef APP_HC05_H
#define APP_HC05_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Configure UART7 and the ring buffers. Call once, before the console task
 *  starts. Cannot fail in a way worth reporting: with no module attached the
 *  transmit side simply goes nowhere. */
void hc05_init(void);

/** True once hc05_init() has run (so early printf mirroring is a no-op). */
bool hc05_ready(void);

/** Queue bytes for transmission. Never blocks; returns the number accepted,
 *  which is short of `len` when the ring is full. */
size_t hc05_write(const void *data, size_t len);

/** printf to the module. Output is truncated to an internal line buffer. */
void hc05_printf(const char *format, ...);

/** Mirror one character of the debug console. Separate from hc05_write() so
 *  it can be muted at runtime without affecting console replies, and so it
 *  is cheap enough to sit in __io_putchar(). */
void hc05_log_putchar(char value);

/** Enable/disable the debug-log mirror (console replies are unaffected). */
void hc05_set_log_mirror(bool enabled);
bool hc05_get_log_mirror(void);

/** Pop one complete line (CR/LF stripped) typed by the phone into `out`.
 *  Returns false when no full line is buffered yet. Non-blocking. */
bool hc05_read_line(char *out, size_t size);

/** Bytes dropped because the transmit ring was full, for diagnostics. */
uint32_t hc05_dropped(void);

#endif /* APP_HC05_H */
