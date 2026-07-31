/**
 * esp01.c  —  ESP-01 (ESP8266) AT-command WiFi driver on USART6.
 *
 * Receive path: USART6 RXNE interrupt pushes bytes into a lock-free ring;
 * task-level code drains it. Deliberately NOT DMA — a DMA target buffer would
 * need MPU/cache work (AGENTS.md §7) for a stream that is only ~11.5 kB/s, and
 * the ISR costs well under 1% of the M7 at 115200 baud.
 *
 * Receive model: the module is put in PASSIVE mode (AT+CIPRECVMODE=1) so it
 * buffers socket data until we ask for it with AT+CIPRECVDATA. This is not an
 * optimisation but a correctness requirement — the ESP-01's 8-pin header has
 * no RTS/CTS, so in the default "push" mode a burst (a 25 kB logo PNG) would
 * overrun the UART and silently corrupt the TLS stream with no way to recover.
 * Firmware too old for AT+CIPRECVMODE is reported as unusable rather than
 * limped along; see AGENTS.md §8.
 *
 * Concurrency: one AT channel shared by the net task (TLS client) and the web
 * task (HTTP server), so every exchange runs under `at_lock`. All operations
 * are bounded — passive receive means we never park on the channel waiting for
 * a peer to speak.
 */
#include "app/esp01.h"

#include "app/config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "main.h"
#include "cmsis_os.h"

extern UART_HandleTypeDef huart6;

/* ---- Receive ring (ISR producer, task consumer) -------------------------- */

#define RX_RING_SIZE   4096U          /* must stay a power of two */
#define RX_RING_MASK   (RX_RING_SIZE - 1U)

static volatile uint8_t  rx_ring[RX_RING_SIZE];
static volatile uint16_t rx_head;     /* written by the ISR only  */
static volatile uint16_t rx_tail;     /* written by tasks only    */
static volatile uint32_t rx_dropped;

#define LINE_MAX  192U

/* ---- Module / link state ------------------------------------------------- */

typedef enum
{
  LINK_FREE = 0,
  LINK_CLIENT,     /* we opened it with AT+CIPSTART */
  LINK_SERVER      /* the AT server accepted it     */
} link_role_t;

typedef struct
{
  link_role_t role;
  bool open;             /* peer still connected                       */
  bool accept_pending;   /* inbound, not yet handed to esp01_accept()  */
} link_state_t;

static link_state_t links[ESP01_MAX_LINKS];

static bool module_present;
static bool wifi_connected;
static bool wifi_has_ip;

static char status_ip[16]      = "0.0.0.0";
static char status_gateway[16] = "0.0.0.0";
static char status_netmask[16] = "0.0.0.0";
static char status_mac[18]     = "00:00:00:00:00:00";

static osMutexId at_lock;

/* ---- Time helpers -------------------------------------------------------- */

static uint32_t now_ms(void)
{
  return osKernelSysTick();
}

static bool expired(uint32_t deadline)
{
  return (int32_t)(now_ms() - deadline) >= 0;
}

static void lock(void)
{
  if (at_lock != NULL) osMutexWait(at_lock, osWaitForever);
}

static void unlock(void)
{
  if (at_lock != NULL) osMutexRelease(at_lock);
}

/* ---- USART6 raw I/O ------------------------------------------------------ */

void USART6_IRQHandler(void)
{
  USART_TypeDef *uart = USART6;
  uint32_t isr = uart->ISR;

  if ((isr & USART_ISR_RXNE) != 0U)
  {
    uint8_t byte = (uint8_t)uart->RDR;          /* reading RDR clears RXNE */
    uint16_t next = (uint16_t)((rx_head + 1U) & RX_RING_MASK);
    if (next != rx_tail)
    {
      rx_ring[rx_head] = byte;
      rx_head = next;
    }
    else
    {
      ++rx_dropped;    /* consumer fell behind; the AT parser will time out */
    }
  }

  /* Overrun latches and blocks further RXNE until acknowledged. */
  if ((isr & (USART_ISR_ORE | USART_ISR_NE | USART_ISR_FE | USART_ISR_PE)) != 0U)
  {
    uart->ICR = USART_ICR_ORECF | USART_ICR_NCF | USART_ICR_FECF |
                USART_ICR_PECF;
  }
}

static void rx_reset(void)
{
  taskENTER_CRITICAL();
  rx_tail = rx_head;
  taskEXIT_CRITICAL();
}

static bool rx_pop(uint8_t *out)
{
  if (rx_tail == rx_head) return false;
  *out = rx_ring[rx_tail];
  rx_tail = (uint16_t)((rx_tail + 1U) & RX_RING_MASK);
  return true;
}

/* Pop one byte, sleeping until `deadline` for it to arrive. */
static bool rx_pop_wait(uint8_t *out, uint32_t deadline)
{
  for (;;)
  {
    if (rx_pop(out)) return true;
    if (expired(deadline)) return false;
    osDelay(1);
  }
}

static void uart_write(const void *data, size_t len)
{
  HAL_UART_Transmit(&huart6, (uint8_t *)data, (uint16_t)len,
                    (uint32_t)len * 2U + 200U);
}

static void uart_write_line(const char *text)
{
  uart_write(text, strlen(text));
  uart_write("\r\n", 2);
}

/* ---- URC handling --------------------------------------------------------
 * Unsolicited result codes can land between any two lines of a command's
 * response, so every line read anywhere is offered here first. */

static void handle_urc(const char *line)
{
  if (strcmp(line, "WIFI CONNECTED") == 0)
  {
    wifi_connected = true;
    return;
  }
  if (strcmp(line, "WIFI GOT IP") == 0)
  {
    wifi_has_ip = true;
    return;
  }
  if (strcmp(line, "WIFI DISCONNECT") == 0)
  {
    wifi_connected = false;
    wifi_has_ip = false;
    return;
  }

  /* "<id>,CONNECT" / "<id>,CLOSED" with AT+CIPMUX=1. */
  if (line[0] >= '0' && line[0] <= '9' && line[1] == ',')
  {
    int id = line[0] - '0';
    if (id < 0 || id >= ESP01_MAX_LINKS) return;

    if (strcmp(line + 2, "CONNECT") == 0)
    {
      links[id].open = true;
      if (links[id].role == LINK_FREE)
      {
        /* Nobody reserved this id, so it is an inbound server connection. */
        links[id].role = LINK_SERVER;
        links[id].accept_pending = true;
      }
    }
    else if (strcmp(line + 2, "CLOSED") == 0 ||
             strcmp(line + 2, "CONNECT FAIL") == 0)
    {
      /* Stay allocated: the owner still holds the id and must close it, or
       * we would hand the same id to a new connection underneath them. */
      links[id].open = false;
    }
  }
}

/* Read one line into `out` (CR/LF stripped).
 *
 * `stop_at_colon` additionally terminates on ':' — needed for
 * "+CIPRECVDATA,<len>:<raw bytes>", whose payload is binary and would
 * otherwise be eaten looking for a newline. `*terminator` reports which
 * character ended the line ('\n' or ':').
 *
 * Returns false on timeout. */
static bool read_line(char *out, size_t size, uint32_t deadline,
                      bool stop_at_colon, char *terminator)
{
  size_t used = 0;
  for (;;)
  {
    uint8_t byte;
    if (!rx_pop_wait(&byte, deadline)) return false;

    if (byte == '\n')
    {
      out[used] = '\0';
      if (terminator != NULL) *terminator = '\n';
      return true;
    }
    if (byte == '\r') continue;
    if (stop_at_colon && byte == ':')
    {
      out[used] = '\0';
      if (terminator != NULL) *terminator = ':';
      return true;
    }
    if (used + 1U < size) out[used++] = (char)byte;
  }
}

/* Drain and dispatch whatever complete lines are already buffered. */
static void pump_urcs(void)
{
  char line[LINE_MAX];
  while (rx_tail != rx_head)
  {
    char terminator = '\n';
    /* Deadline of "now" keeps this non-blocking: a partial trailing line is
     * left in the ring for the next call. */
    uint16_t saved_tail = rx_tail;
    if (!read_line(line, sizeof(line), now_ms(), false, &terminator))
    {
      rx_tail = saved_tail;
      return;
    }
    if (line[0] != '\0') handle_urc(line);
  }
}

/* ---- AT command engine --------------------------------------------------- */

/* Send `command` and read lines until a final result code.
 * Returns 0 on "OK", -1 on "ERROR"/"FAIL"/timeout.
 * When `capture` is non-NULL, informational lines are appended to it
 * (newline separated) for the caller to parse. */
static int at_command(const char *command, uint32_t timeout_ms, char *capture,
                      size_t capture_size)
{
  if (capture != NULL && capture_size > 0U) capture[0] = '\0';
  uart_write_line(command);

  uint32_t deadline = now_ms() + timeout_ms;
  char line[LINE_MAX];
  for (;;)
  {
    if (!read_line(line, sizeof(line), deadline, false, NULL)) return -1;
    if (line[0] == '\0') continue;

    if (strcmp(line, "OK") == 0) return 0;
    if (strcmp(line, "ERROR") == 0 || strcmp(line, "FAIL") == 0 ||
        strcmp(line, "SEND FAIL") == 0)
    {
      return -1;
    }
    /* Echo of our own command (should not happen after ATE0). */
    if (strncmp(line, "AT", 2) == 0) continue;

    handle_urc(line);

    if (capture != NULL && capture_size > 0U)
    {
      size_t used = strlen(capture);
      size_t room = capture_size - used;
      if (room > 1U) snprintf(capture + used, room, "%s\n", line);
    }
  }
}

/* ---- Init ---------------------------------------------------------------- */

static void log_firmware(void)
{
  char capture[LINE_MAX * 2];
  if (at_command("AT+GMR", 2000U, capture, sizeof(capture)) == 0)
  {
    /* Keep it to the first line — that is the AT version. */
    char *newline = strchr(capture, '\n');
    if (newline != NULL) *newline = '\0';
    printf("[esp] firmware: %s\r\n", capture);
  }
}

/* Try `preferred` (the _CUR form, which does not wear out the module's flash)
 * and fall back to the legacy name on older firmware. */
static int at_command_pref(const char *preferred, const char *fallback,
                           uint32_t timeout_ms)
{
  if (at_command(preferred, timeout_ms, NULL, 0) == 0) return 0;
  return at_command(fallback, timeout_ms, NULL, 0);
}

bool esp01_init(void)
{
  if (at_lock == NULL)
  {
    osMutexDef(esp01AtLock);
    at_lock = osMutexCreate(osMutex(esp01AtLock));
  }

  lock();

  /* Take over USART6 receive. main.c already ran MX_USART6_UART_Init(), so
   * the pins, clock and baud are set; we only add the RX interrupt. Priority
   * 6 is numerically above configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY (5),
   * so it stays FreeRTOS-safe and below ETH/LTDC. */
  HAL_NVIC_SetPriority(USART6_IRQn, 6, 0);
  HAL_NVIC_EnableIRQ(USART6_IRQn);
  __HAL_UART_ENABLE_IT(&huart6, UART_IT_RXNE);

  rx_reset();
  memset(links, 0, sizeof(links));
  module_present = false;
  wifi_connected = false;
  wifi_has_ip = false;

  /* A module that is absent, unpowered or mis-wired simply never answers. */
  bool answered = false;
  for (int attempt = 0; attempt < 3 && !answered; ++attempt)
  {
    if (at_command("AT", 1000U, NULL, 0) == 0) answered = true;
  }
  if (!answered)
  {
    printf("[esp] no module on USART6 (check 3V3 supply, CH_PD, D0/D1)\r\n");
    unlock();
    return false;
  }

  at_command("ATE0", 1000U, NULL, 0);          /* stop echoing our commands */
  log_firmware();

  if (at_command_pref("AT+CWMODE_CUR=1", "AT+CWMODE=1", 2000U) != 0)
  {
    printf("[esp] station mode refused\r\n");
    unlock();
    return false;
  }

  if (at_command("AT+CIPMUX=1", 2000U, NULL, 0) != 0)
  {
    printf("[esp] AT+CIPMUX=1 refused\r\n");
    unlock();
    return false;
  }

  /* Non-negotiable: see the passive-mode note at the top of this file. */
  if (at_command("AT+CIPRECVMODE=1", 2000U, NULL, 0) != 0)
  {
    printf("[esp] AT+CIPRECVMODE unsupported - this AT firmware is too old.\r\n");
    printf("[esp] Without passive receive the UART overruns on large "
           "responses; WiFi disabled. Flash ESP8266 AT >= 1.7.\r\n");
    unlock();
    return false;
  }

  module_present = true;
  unlock();
  printf("[esp] ready (station mode, passive receive)\r\n");
  return true;
}

bool esp01_present(void)
{
  return module_present;
}

/* ---- Association --------------------------------------------------------- */

/* Bounded copy, committed inside a critical section so esp01_get_status()
 * (called from the UI task on every refresh) can read these without taking
 * the AT mutex - waiting on that behind a 15 s AT+CIPSTART would freeze LVGL.
 *
 * Using snprintf("%s") here would also make GCC warn about a truncation that
 * is exactly what we want for a malformed module response. */
static void copy_field(char *dest, size_t dest_size, const char *source)
{
  size_t length = strlen(source);
  if (length >= dest_size) length = dest_size - 1U;
  taskENTER_CRITICAL();
  memcpy(dest, source, length);
  dest[length] = '\0';
  taskEXIT_CRITICAL();
}

static void read_field(char *dest, size_t dest_size, const char *source)
{
  taskENTER_CRITICAL();
  size_t length = strlen(source);
  if (length >= dest_size) length = dest_size - 1U;
  memcpy(dest, source, length);
  dest[length] = '\0';
  taskEXIT_CRITICAL();
}

static void parse_cifsr(const char *capture)
{
  /* Lines look like: +CIPSTA_CUR:ip:"192.168.1.42"  /  +CIPSTAMAC:"a0:.." */
  const char *cursor = capture;
  while (cursor != NULL && *cursor != '\0')
  {
    const char *quote = strchr(cursor, '"');
    const char *end = (quote != NULL) ? strchr(quote + 1, '"') : NULL;
    if (quote == NULL || end == NULL) break;

    size_t length = (size_t)(end - quote - 1);
    char value[24];
    if (length >= sizeof(value)) length = sizeof(value) - 1U;
    memcpy(value, quote + 1, length);
    value[length] = '\0';

    /* The "< quote" guards keep a keyword from a *later* line in the capture
     * from claiming this line's value. */
    if (strstr(cursor, "ip:") != NULL && strstr(cursor, "ip:") < quote)
    {
      copy_field(status_ip, sizeof(status_ip), value);
    }
    else if (strstr(cursor, "gateway:") != NULL &&
             strstr(cursor, "gateway:") < quote)
    {
      copy_field(status_gateway, sizeof(status_gateway), value);
    }
    else if (strstr(cursor, "netmask:") != NULL &&
             strstr(cursor, "netmask:") < quote)
    {
      copy_field(status_netmask, sizeof(status_netmask), value);
    }
    else if (strstr(cursor, "MAC") != NULL && strstr(cursor, "MAC") < quote)
    {
      copy_field(status_mac, sizeof(status_mac), value);
    }

    cursor = strchr(end + 1, '\n');
    if (cursor != NULL) ++cursor;
  }
}

static void refresh_status(void)
{
  char capture[320];
  if (at_command("AT+CIPSTA_CUR?", 2000U, capture, sizeof(capture)) != 0)
  {
    at_command("AT+CIPSTA?", 2000U, capture, sizeof(capture));
  }
  parse_cifsr(capture);

  if (at_command("AT+CIPSTAMAC_CUR?", 2000U, capture, sizeof(capture)) != 0)
  {
    at_command("AT+CIPSTAMAC?", 2000U, capture, sizeof(capture));
  }
  parse_cifsr(capture);
}

bool esp01_join(const char *ssid, const char *password)
{
  if (!module_present) return false;

  lock();

  char command[160];
  snprintf(command, sizeof(command), "AT+CWJAP_CUR=\"%s\",\"%s\"", ssid,
           password);
  int result = at_command(command, 20000U, NULL, 0);
  if (result != 0)
  {
    snprintf(command, sizeof(command), "AT+CWJAP=\"%s\",\"%s\"", ssid,
             password);
    result = at_command(command, 20000U, NULL, 0);
  }

  if (result != 0)
  {
    printf("[esp] join \"%s\" failed\r\n", ssid);
    unlock();
    return false;
  }

  /* CWJAP returns once associated; the lease may land a moment later. */
  uint32_t deadline = now_ms() + 8000U;
  while (!wifi_has_ip && !expired(deadline))
  {
    pump_urcs();
    osDelay(50);
  }

  refresh_status();
  bool bound = (strcmp(status_ip, "0.0.0.0") != 0 && status_ip[0] != '\0');
  wifi_connected = bound;
  wifi_has_ip = bound;

  if (bound)
  {
    printf("[esp] joined \"%s\", IP = %s\r\n", ssid, status_ip);
  }
  else
  {
    printf("[esp] joined \"%s\" but no DHCP lease\r\n", ssid);
  }

  unlock();
  return bound;
}

bool esp01_is_connected(void)
{
  return module_present && wifi_connected && wifi_has_ip;
}

void esp01_get_status(char *ip, size_t ip_size, char *gateway,
                      size_t gateway_size, char *netmask, size_t netmask_size,
                      char *mac, size_t mac_size)
{
  /* Deliberately does NOT take the AT mutex - see copy_field(). */
  if (ip != NULL) read_field(ip, ip_size, status_ip);
  if (gateway != NULL) read_field(gateway, gateway_size, status_gateway);
  if (netmask != NULL) read_field(netmask, netmask_size, status_netmask);
  if (mac != NULL) read_field(mac, mac_size, status_mac);
}

/* ---- Client connections --------------------------------------------------- */

int esp01_connect(const char *host, uint16_t port)
{
  if (!esp01_is_connected()) return ESP01_ERR;

  lock();
  pump_urcs();   /* learn about inbound links before choosing a free id */

  for (int id = 0; id < ESP01_MAX_LINKS; ++id)
  {
    if (links[id].role != LINK_FREE) continue;

    links[id].role = LINK_CLIENT;
    links[id].open = false;
    links[id].accept_pending = false;

    char command[160];
    snprintf(command, sizeof(command), "AT+CIPSTART=%d,\"TCP\",\"%s\",%u", id,
             host, (unsigned)port);
    /* Generous: covers DNS resolution plus the TCP handshake. */
    if (at_command(command, 15000U, NULL, 0) == 0)
    {
      links[id].open = true;
      unlock();
      return id;
    }

    links[id].role = LINK_FREE;
    /* "ALREADY CONNECTED" means the module handed this id to an inbound
     * client between our pump and the CIPSTART - try the next slot. */
  }

  unlock();
  return ESP01_ERR;
}

int esp01_send(int link, const uint8_t *data, size_t len)
{
  if (link < 0 || link >= ESP01_MAX_LINKS) return ESP01_ERR;

  lock();
  if (!links[link].open)
  {
    unlock();
    return ESP01_ERR;
  }

  size_t sent = 0;
  while (sent < len)
  {
    size_t chunk = len - sent;
    if (chunk > ESP01_SEND_CHUNK) chunk = ESP01_SEND_CHUNK;

    char command[48];
    snprintf(command, sizeof(command), "AT+CIPSEND=%d,%u", link,
             (unsigned)chunk);
    uart_write_line(command);

    /* Wait for the '>' prompt, tolerating the "OK" some firmware emits
     * first. An ERROR here means the link died under us. */
    uint32_t deadline = now_ms() + 5000U;
    bool prompted = false;
    char pending[LINE_MAX];
    size_t used = 0;
    while (!prompted)
    {
      uint8_t byte;
      if (!rx_pop_wait(&byte, deadline)) break;
      if (byte == '>')
      {
        prompted = true;
        break;
      }
      if (byte == '\n' || byte == '\r')
      {
        pending[used] = '\0';
        if (used > 0U)
        {
          if (strcmp(pending, "ERROR") == 0 || strcmp(pending, "FAIL") == 0)
          {
            break;
          }
          handle_urc(pending);
        }
        used = 0;
      }
      else if (used + 1U < sizeof(pending))
      {
        pending[used++] = (char)byte;
      }
    }

    if (!prompted)
    {
      links[link].open = false;
      unlock();
      return ESP01_ERR;
    }

    uart_write(data + sent, chunk);

    /* The module answers "SEND OK" (or "SEND FAIL") once it has queued it. */
    deadline = now_ms() + 10000U;
    bool acknowledged = false;
    for (;;)
    {
      char line[LINE_MAX];
      if (!read_line(line, sizeof(line), deadline, false, NULL)) break;
      if (line[0] == '\0') continue;
      if (strcmp(line, "SEND OK") == 0)
      {
        acknowledged = true;
        break;
      }
      if (strcmp(line, "SEND FAIL") == 0 || strcmp(line, "ERROR") == 0) break;
      handle_urc(line);
    }

    if (!acknowledged)
    {
      links[link].open = false;
      unlock();
      return ESP01_ERR;
    }

    sent += chunk;
  }

  unlock();
  return (int)sent;
}

int esp01_recv(int link, uint8_t *data, size_t len)
{
  if (link < 0 || link >= ESP01_MAX_LINKS || len == 0U) return ESP01_ERR;

  lock();
  pump_urcs();

  if (links[link].role == LINK_FREE)
  {
    unlock();
    return ESP01_ERR;
  }

  /* One AT+CIPRECVDATA can return at most ESP01_SEND_CHUNK bytes. */
  size_t want = len;
  if (want > ESP01_SEND_CHUNK) want = ESP01_SEND_CHUNK;

  char command[48];
  snprintf(command, sizeof(command), "AT+CIPRECVDATA=%d,%u", link,
           (unsigned)want);
  uart_write_line(command);

  uint32_t deadline = now_ms() + 5000U;
  int received = ESP01_WOULDBLOCK;
  bool finished = false;

  while (!finished)
  {
    char line[LINE_MAX];
    char terminator = '\n';
    if (!read_line(line, sizeof(line), deadline, true, &terminator))
    {
      links[link].open = false;
      unlock();
      return ESP01_ERR;
    }
    if (line[0] == '\0') continue;

    /* "+CIPRECVDATA,<count>:" then exactly <count> raw bytes. */
    if (terminator == ':' && strncmp(line, "+CIPRECVDATA", 12) == 0)
    {
      const char *comma = strchr(line, ',');
      long count = (comma != NULL) ? strtol(comma + 1, NULL, 10) : 0;
      if (count < 0 || (size_t)count > want)
      {
        links[link].open = false;
        unlock();
        return ESP01_ERR;
      }

      for (long i = 0; i < count; ++i)
      {
        uint8_t byte;
        if (!rx_pop_wait(&byte, deadline))
        {
          links[link].open = false;
          unlock();
          return ESP01_ERR;
        }
        data[i] = byte;
      }
      received = (int)count;
      continue;   /* the trailing "OK" still has to be consumed */
    }

    if (strcmp(line, "OK") == 0)
    {
      finished = true;
    }
    else if (strcmp(line, "ERROR") == 0 || strcmp(line, "FAIL") == 0)
    {
      /* Asking a closed link for data errors out; that is the normal way a
       * drained connection reports EOF. */
      received = ESP01_ERR;
      finished = true;
    }
    else
    {
      handle_urc(line);
    }
  }

  if (received == 0) received = ESP01_WOULDBLOCK;

  /* Closed by the peer and nothing left buffered - real end of stream. */
  if (received == ESP01_WOULDBLOCK && !links[link].open) received = ESP01_ERR;

  unlock();
  return received;
}

void esp01_close(int link)
{
  if (link < 0 || link >= ESP01_MAX_LINKS) return;

  lock();
  if (links[link].open)
  {
    char command[32];
    snprintf(command, sizeof(command), "AT+CIPCLOSE=%d", link);
    at_command(command, 5000U, NULL, 0);
  }
  links[link].role = LINK_FREE;
  links[link].open = false;
  links[link].accept_pending = false;
  unlock();
}

/* ---- Server -------------------------------------------------------------- */

bool esp01_listen(uint16_t port)
{
  if (!esp01_is_connected()) return false;

  lock();
  char command[48];

  /* Drop idle clients so a stuck browser tab cannot hold one of the four
   * link slots forever. */
  at_command("AT+CIPSTO=15", 2000U, NULL, 0);

  snprintf(command, sizeof(command), "AT+CIPSERVER=1,%u", (unsigned)port);
  bool ok = (at_command(command, 5000U, NULL, 0) == 0);
  unlock();

  if (!ok) printf("[esp] AT+CIPSERVER on port %u failed\r\n", (unsigned)port);
  return ok;
}

void esp01_listen_stop(void)
{
  if (!module_present) return;

  lock();
  at_command("AT+CIPSERVER=0", 5000U, NULL, 0);
  unlock();
}

int esp01_accept(void)
{
  if (!module_present) return ESP01_ERR;

  lock();
  pump_urcs();

  int found = ESP01_ERR;
  for (int id = 0; id < ESP01_MAX_LINKS; ++id)
  {
    if (links[id].role == LINK_SERVER && links[id].accept_pending)
    {
      links[id].accept_pending = false;
      found = id;
      break;
    }
  }

  unlock();
  return found;
}
