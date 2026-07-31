/**
 * hc05.c  —  HC-05 Bluetooth SPP driver on UART7.
 *
 * See hc05.h for the contract and wiring. Two design points worth keeping:
 *
 * 1. UART7 is brought up by hand here (RCC/GPIO/NVIC + a HAL handle we own)
 *    instead of through CubeMX. USART6 went to the ESP-01 and USART1 is the
 *    ST-Link console, so PF7/PF6 on the Arduino analog header were the
 *    remaining option; they are declared as ADC3 inputs in the .ioc but
 *    nothing in the app uses ADC3.
 *
 * 2. Transmit is interrupt-driven with drop-on-full. printf() is mirrored to
 *    this module from every task in the system, and the HC-05's factory baud
 *    is 9600 - a blocking write would have turned the debug console into a
 *    system-wide brake. Losing log bytes is strictly better than stalling the
 *    UI or the TLS client, so a full ring drops and counts.
 */
#include "app/hc05.h"

#include "app/config.h"

#include <stdio.h>
#include <string.h>

#include "main.h"
#include "FreeRTOS.h"
#include "task.h"

/* Ring sizes must stay powers of two. TX is generous because boot logs
 * arrive in a burst far faster than 9600 baud drains them. */
#define TX_RING_SIZE  2048U
#define TX_RING_MASK  (TX_RING_SIZE - 1U)
#define RX_RING_SIZE  256U
#define RX_RING_MASK  (RX_RING_SIZE - 1U)

static UART_HandleTypeDef huart7;

static volatile uint8_t  tx_ring[TX_RING_SIZE];
static volatile uint16_t tx_head;
static volatile uint16_t tx_tail;
static volatile uint32_t tx_dropped;

static volatile uint8_t  rx_ring[RX_RING_SIZE];
static volatile uint16_t rx_head;
static volatile uint16_t rx_tail;

static bool initialized;
static bool log_mirror = (HC05_LOG_MIRROR_DEFAULT != 0);

/* ---- Interrupt ----------------------------------------------------------- */

void UART7_IRQHandler(void)
{
  USART_TypeDef *uart = UART7;
  uint32_t isr = uart->ISR;

  if ((isr & USART_ISR_RXNE) != 0U)
  {
    uint8_t byte = (uint8_t)uart->RDR;           /* reading RDR clears RXNE */
    uint16_t next = (uint16_t)((rx_head + 1U) & RX_RING_MASK);
    if (next != rx_tail)
    {
      rx_ring[rx_head] = byte;
      rx_head = next;
    }
    /* else: the user typed faster than the console task reads - drop. */
  }

  /* TXE stays enabled only while there is something to send. */
  if ((isr & USART_ISR_TXE) != 0U && (uart->CR1 & USART_CR1_TXEIE) != 0U)
  {
    if (tx_tail == tx_head)
    {
      uart->CR1 &= ~USART_CR1_TXEIE;
    }
    else
    {
      uart->TDR = tx_ring[tx_tail];
      tx_tail = (uint16_t)((tx_tail + 1U) & TX_RING_MASK);
    }
  }

  if ((isr & (USART_ISR_ORE | USART_ISR_NE | USART_ISR_FE | USART_ISR_PE)) != 0U)
  {
    uart->ICR = USART_ICR_ORECF | USART_ICR_NCF | USART_ICR_FECF |
                USART_ICR_PECF;
  }
}

/* ---- Init ---------------------------------------------------------------- */

void hc05_init(void)
{
  if (initialized) return;

  __HAL_RCC_UART7_CLK_ENABLE();
  __HAL_RCC_GPIOF_CLK_ENABLE();

  /* PF7 = UART7_TX, PF6 = UART7_RX, both AF8. They power up as analog
   * (ADC3_IN5/IN4 per the .ioc) and are reclaimed here. */
  GPIO_InitTypeDef gpio = { 0 };
  gpio.Pin = GPIO_PIN_6 | GPIO_PIN_7;
  gpio.Mode = GPIO_MODE_AF_PP;
  gpio.Pull = GPIO_PULLUP;      /* idle-high line, so a floating TX is safe */
  gpio.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  gpio.Alternate = GPIO_AF8_UART7;
  HAL_GPIO_Init(GPIOF, &gpio);

  huart7.Instance = UART7;
  huart7.Init.BaudRate = HC05_BAUD;
  huart7.Init.WordLength = UART_WORDLENGTH_8B;
  huart7.Init.StopBits = UART_STOPBITS_1;
  huart7.Init.Parity = UART_PARITY_NONE;
  huart7.Init.Mode = UART_MODE_TX_RX;
  huart7.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart7.Init.OverSampling = UART_OVERSAMPLING_16;
  huart7.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
  huart7.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  if (HAL_UART_Init(&huart7) != HAL_OK)
  {
    printf("[bt] UART7 init failed\r\n");
    return;
  }

  /* Priority 6: numerically above configLIBRARY_MAX_SYSCALL_INTERRUPT_
   * PRIORITY (5), so it is FreeRTOS-safe, and below ETH/LTDC. */
  HAL_NVIC_SetPriority(UART7_IRQn, 6, 0);
  HAL_NVIC_EnableIRQ(UART7_IRQn);
  __HAL_UART_ENABLE_IT(&huart7, UART_IT_RXNE);

  initialized = true;
  printf("[bt] HC-05 ready on UART7 @ %u baud (A4=TX, A5=RX)\r\n",
         (unsigned)HC05_BAUD);
}

bool hc05_ready(void)
{
  return initialized;
}

/* ---- Transmit ------------------------------------------------------------ */

size_t hc05_write(const void *data, size_t len)
{
  if (!initialized) return 0;

  const uint8_t *bytes = (const uint8_t *)data;
  size_t accepted = 0;

  /* Critical sections rather than a mutex: this runs from __io_putchar(),
   * which printf() may reach before the scheduler is even started. */
  taskENTER_CRITICAL();
  for (size_t i = 0; i < len; ++i)
  {
    uint16_t next = (uint16_t)((tx_head + 1U) & TX_RING_MASK);
    if (next == tx_tail)
    {
      tx_dropped += (uint32_t)(len - i);
      break;
    }
    tx_ring[tx_head] = bytes[i];
    tx_head = next;
    ++accepted;
  }

  if (accepted > 0U)
  {
    UART7->CR1 |= USART_CR1_TXEIE;   /* kick the drain */
  }
  taskEXIT_CRITICAL();

  return accepted;
}

void hc05_printf(const char *format, ...)
{
  if (!initialized) return;

  char line[192];
  va_list args;
  va_start(args, format);
  int length = vsnprintf(line, sizeof(line), format, args);
  va_end(args);

  if (length > 0)
  {
    if ((size_t)length >= sizeof(line)) length = (int)sizeof(line) - 1;
    hc05_write(line, (size_t)length);
  }
}

void hc05_log_putchar(char value)
{
  if (!initialized || !log_mirror) return;
  hc05_write(&value, 1U);
}

void hc05_set_log_mirror(bool enabled)
{
  log_mirror = enabled;
}

bool hc05_get_log_mirror(void)
{
  return log_mirror;
}

uint32_t hc05_dropped(void)
{
  return tx_dropped;
}

/* ---- Receive ------------------------------------------------------------- */

bool hc05_read_line(char *out, size_t size)
{
  if (!initialized || size == 0U) return false;

  /* Only consume the bytes once a full line is present, so a half-typed
   * command is left alone until its terminator arrives. */
  uint16_t scan = rx_tail;
  bool complete = false;
  while (scan != rx_head)
  {
    uint8_t byte = rx_ring[scan];
    if (byte == '\n' || byte == '\r')
    {
      complete = true;
      break;
    }
    scan = (uint16_t)((scan + 1U) & RX_RING_MASK);
  }
  if (!complete) return false;

  size_t used = 0;
  while (rx_tail != rx_head)
  {
    uint8_t byte = rx_ring[rx_tail];
    rx_tail = (uint16_t)((rx_tail + 1U) & RX_RING_MASK);
    if (byte == '\n' || byte == '\r') break;
    if (used + 1U < size) out[used++] = (char)byte;
  }
  out[used] = '\0';

  /* Swallow a CRLF's second byte so it does not read as an empty line. */
  if (rx_tail != rx_head)
  {
    uint8_t peek = rx_ring[rx_tail];
    if (peek == '\n' || peek == '\r')
    {
      rx_tail = (uint16_t)((rx_tail + 1U) & RX_RING_MASK);
    }
  }

  return true;
}
