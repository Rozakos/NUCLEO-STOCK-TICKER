#include "app/watchdog.h"

#include <stdio.h>
#include <string.h>

#include "cmsis_os.h"
#include "stm32f7xx.h"

/* IWDG: LSI ~32 kHz / 256 = 125 Hz, reload 2000 -> 16 s. Long enough to ride
 * out a flash-sector erase (the CPU stalls ~1-2 s on this single-bank part)
 * and a slow TLS handshake, short enough that a hang is not left standing. */
#define IWDG_PRESCALER_256 6U
#define IWDG_RELOAD        2000U

/* How long each loop may go without checking in. NET covers a full batch
 * fetch with every request hitting its 20 s timeout. */
static const uint32_t limits_ms[WATCHDOG_CLIENT_COUNT] = { 15000U, 240000U };
static const char *const names[WATCHDOG_CLIENT_COUNT] = { "UI", "NET" };
static volatile uint32_t last_checkin[WATCHDOG_CLIENT_COUNT];
static volatile int stalled = -1;
static int started;

/* Survives a reset (not zeroed by the startup code, see the .noinit section
 * in STM32F746NGHX_FLASH.ld). Written by the fault handler or by the
 * supervisor right before it lets the watchdog fire. */
#define CRASH_MAGIC 0xC4A5F00DU
typedef struct
{
  uint32_t magic;
  uint32_t pc;
  uint32_t lr;
  uint32_t cfsr;
  uint32_t hfsr;
  uint32_t mmfar;
  uint32_t bfar;
  char kind[16];
  char detail[24];
} crash_record_t;
__attribute__((section(".noinit"))) static crash_record_t crash_record;

static char reset_text[96] = "unknown";

/* ---- raw console (USART1, ST-Link VCP) ----------------------------------
 * printf goes through HAL_UART_Transmit, whose handle lock may be held by the
 * very task that faulted; poll the registers directly instead. */
static void raw_putc(char c)
{
  for (uint32_t spin = 0; spin < 200000U && !(USART1->ISR & USART_ISR_TXE); ++spin)
  {
  }
  USART1->TDR = (uint8_t)c;
}

static void raw_puts(const char *s)
{
  while (s != NULL && *s != '\0') raw_putc(*s++);
}

static void raw_hex(uint32_t value)
{
  static const char digits[] = "0123456789ABCDEF";
  raw_puts("0x");
  for (int shift = 28; shift >= 0; shift -= 4) raw_putc(digits[(value >> shift) & 0xFU]);
}

void fault_record_and_reset(const char *kind, uint32_t pc, uint32_t lr,
                            const char *detail)
{
  __disable_irq();
  crash_record.magic = CRASH_MAGIC;
  crash_record.pc = pc;
  crash_record.lr = lr;
  crash_record.cfsr = SCB->CFSR;
  crash_record.hfsr = SCB->HFSR;
  crash_record.mmfar = SCB->MMFAR;
  crash_record.bfar = SCB->BFAR;
  strncpy(crash_record.kind, kind != NULL ? kind : "fault", sizeof(crash_record.kind) - 1U);
  crash_record.kind[sizeof(crash_record.kind) - 1U] = '\0';
  strncpy(crash_record.detail, detail != NULL ? detail : "", sizeof(crash_record.detail) - 1U);
  crash_record.detail[sizeof(crash_record.detail) - 1U] = '\0';
  SCB_CleanDCache();   /* the record must reach SRAM before the reset */

  raw_puts("\r\n[fatal] ");
  raw_puts(crash_record.kind);
  if (crash_record.detail[0] != '\0') { raw_puts(" "); raw_puts(crash_record.detail); }
  raw_puts(" pc="); raw_hex(pc);
  raw_puts(" lr="); raw_hex(lr);
  raw_puts(" cfsr="); raw_hex(crash_record.cfsr);
  raw_puts(" hfsr="); raw_hex(crash_record.hfsr);
  raw_puts(" mmfar="); raw_hex(crash_record.mmfar);
  raw_puts(" bfar="); raw_hex(crash_record.bfar);
  raw_puts("\r\n[fatal] resetting. Decode pc/lr with arm-none-eabi-addr2line -e Debug/NUCLEO-STOCK-TICKER.elf\r\n");
  for (volatile uint32_t i = 0; i < 2000000U; ++i)
  {
  }
  NVIC_SystemReset();
  for (;;)
  {
  }
}

void watchdog_report_reset_cause(void)
{
  uint32_t csr = RCC->CSR;
  RCC->CSR |= RCC_CSR_RMVF;

  if (crash_record.magic == CRASH_MAGIC)
  {
    crash_record.kind[sizeof(crash_record.kind) - 1U] = '\0';
    crash_record.detail[sizeof(crash_record.detail) - 1U] = '\0';
    snprintf(reset_text, sizeof(reset_text), "%s %s at pc 0x%08lX (lr 0x%08lX)",
             crash_record.kind, crash_record.detail,
             (unsigned long)crash_record.pc, (unsigned long)crash_record.lr);
    printf("[boot] last reset: %s, cfsr=0x%08lX hfsr=0x%08lX mmfar=0x%08lX bfar=0x%08lX\r\n",
           reset_text, (unsigned long)crash_record.cfsr,
           (unsigned long)crash_record.hfsr, (unsigned long)crash_record.mmfar,
           (unsigned long)crash_record.bfar);
    crash_record.magic = 0U;
    return;
  }
  const char *cause = (csr & RCC_CSR_IWDGRSTF) ? "watchdog (task hung)"
                    : (csr & RCC_CSR_WWDGRSTF) ? "window watchdog"
                    : (csr & RCC_CSR_LPWRRSTF) ? "low-power reset"
                    : (csr & RCC_CSR_BORRSTF)  ? "power-on / brown-out"
                    : (csr & RCC_CSR_SFTRSTF)  ? "software reset"
                    : (csr & RCC_CSR_PINRSTF)  ? "reset button / debugger"
                    : "unknown";
  snprintf(reset_text, sizeof(reset_text), "%s", cause);
  printf("[boot] last reset: %s\r\n", reset_text);
}

const char *watchdog_last_reset_text(void)
{
  return reset_text;
}

void watchdog_start(void)
{
  uint32_t now = osKernelSysTick();
  for (int i = 0; i < WATCHDOG_CLIENT_COUNT; ++i) last_checkin[i] = now;
  DBGMCU->APB1FZ |= DBGMCU_APB1_FZ_DBG_IWDG_STOP;   /* halts with the debugger */
  IWDG->KR = 0xCCCCU;                                /* start */
  IWDG->KR = 0x5555U;                                /* unlock PR/RLR */
  IWDG->PR = IWDG_PRESCALER_256;
  IWDG->RLR = IWDG_RELOAD;
  for (uint32_t spin = 0; spin < 100000U && IWDG->SR != 0U; ++spin)
  {
  }
  IWDG->KR = 0xAAAAU;
  started = 1;
  printf("[watchdog] armed: 16 s hardware timeout, UI %lu s / NET %lu s loop limits\r\n",
         (unsigned long)(limits_ms[WATCHDOG_UI] / 1000U),
         (unsigned long)(limits_ms[WATCHDOG_NET] / 1000U));
}

void watchdog_checkin(watchdog_client_t client)
{
  if ((unsigned)client < WATCHDOG_CLIENT_COUNT) last_checkin[client] = osKernelSysTick();
}

void watchdog_service(void)
{
  if (!started) return;
  uint32_t now = osKernelSysTick();
  for (int i = 0; i < WATCHDOG_CLIENT_COUNT; ++i)
  {
    if (now - last_checkin[i] > limits_ms[i])
    {
      if (stalled != i)
      {
        stalled = i;
        /* Stop feeding; leave a note so the next boot says which loop hung. */
        crash_record.magic = CRASH_MAGIC;
        crash_record.pc = 0U;
        crash_record.lr = 0U;
        crash_record.cfsr = crash_record.hfsr = crash_record.mmfar = crash_record.bfar = 0U;
        snprintf(crash_record.kind, sizeof(crash_record.kind), "Watchdog:");
        snprintf(crash_record.detail, sizeof(crash_record.detail), "%s loop stalled", names[i]);
        SCB_CleanDCache();
        printf("[watchdog] %s loop silent for %lu s -- letting the watchdog reset the board\r\n",
               names[i], (unsigned long)((now - last_checkin[i]) / 1000U));
      }
      return;
    }
  }
  IWDG->KR = 0xAAAAU;
}
