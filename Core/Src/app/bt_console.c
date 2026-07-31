/**
 * bt_console.c  —  Bluetooth SPP admin console and price alerts.
 *
 * One task owns both jobs because they share the HC-05 and the same cadence:
 * drain any typed commands, then re-evaluate the alert thresholds.
 *
 * Note newlib-nano has no %f, so every float goes through format_decimal_2()
 * — the same constraint the UI and web pages work under.
 */
#include "app/bt_console.h"

#include "app/config.h"
#include "app/format.h"
#include "app/hc05.h"
#include "app/net_link.h"
#include "app/settings.h"
#include "app/stock_data.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "main.h"

/* Static rather than stack, and looked up one symbol at a time via
 * stock_data_get_symbol(): a full stock_snapshot_t[APP_MAX_SYMBOLS] is
 * ~2.8 KB, which is what forced the UI task's stack up to 3072 words. */
static char symbols[APP_MAX_SYMBOLS][APP_SYMBOL_LENGTH];

/* Edge-trigger state: an alert fires on the crossing, not continuously while
 * the price sits past the threshold. Re-arms when the price comes back. */
static bool above_armed[APP_MAX_SYMBOLS];
static bool below_armed[APP_MAX_SYMBOLS];
static uint32_t alerts_seen_generation;

/* ---- Helpers -------------------------------------------------------------- */

/* The print the user cares about: during pre/after-hours that is the
 * extended quote, matching what the screen and the web page show. */
static float effective_price(const stock_snapshot_t *snapshot)
{
  return (snapshot->ext_state != STOCK_EXT_NONE) ? snapshot->ext_price
                                                 : snapshot->last;
}

static bool find_snapshot(const char *symbol, stock_snapshot_t *out)
{
  return stock_data_get_symbol(symbol, out);
}

/* Case-insensitive word compare against a NUL-terminated token. */
static bool word_is(const char *word, const char *expected)
{
  while (*word != '\0' && *expected != '\0')
  {
    if (tolower((unsigned char)*word) != tolower((unsigned char)*expected))
    {
      return false;
    }
    ++word;
    ++expected;
  }
  return *word == '\0' && *expected == '\0';
}

/* Split off the next whitespace-delimited token, advancing *cursor. */
static char *next_token(char **cursor)
{
  char *scan = *cursor;
  while (*scan == ' ' || *scan == '\t') ++scan;
  if (*scan == '\0')
  {
    *cursor = scan;
    return NULL;
  }

  char *start = scan;
  while (*scan != '\0' && *scan != ' ' && *scan != '\t') ++scan;
  if (*scan != '\0')
  {
    *scan = '\0';
    ++scan;
  }
  *cursor = scan;
  return start;
}

/* ---- Commands ------------------------------------------------------------- */

static void print_help(void)
{
  hc05_printf(
      "\r\nCommands\r\n"
      "  list                 watchlist with live prices\r\n"
      "  status               link, IP, session, uptime\r\n"
      "  add <SYM>            add a symbol\r\n"
      "  del <N>              delete symbol N (see list)\r\n"
      "  shares <N> <QTY>     set shares owned\r\n"
      "  refresh <SEC>        quote refresh interval (15-3600)\r\n"
      "  alert <N> above <P>  notify when price rises past P\r\n"
      "  alert <N> below <P>  notify when price falls past P\r\n"
      "  alert <N> off        clear both thresholds\r\n"
      "  log on|off           mirror the debug console here\r\n"
      "  help                 this text\r\n");
}

static void print_list(void)
{
  size_t count = settings_get_symbols(symbols);
  float shares[APP_MAX_SYMBOLS];
  float above[APP_MAX_SYMBOLS];
  float below[APP_MAX_SYMBOLS];
  settings_get_shares(shares);
  settings_get_alerts(above, below);

  hc05_printf("\r\n #  SYMBOL     PRICE   CHANGE   HOLDING\r\n");

  float portfolio = 0.0f;
  for (size_t i = 0; i < count; ++i)
  {
    stock_snapshot_t snapshot;
    char price[20] = "-";
    char change[20] = "-";
    char holding[24] = "";

    if (find_snapshot(symbols[i], &snapshot) && snapshot.fresh)
    {
      float value = effective_price(&snapshot);
      format_decimal_2(price, sizeof(price), value, 0);
      format_decimal_2(change, sizeof(change),
                       snapshot.ext_state != STOCK_EXT_NONE
                           ? snapshot.ext_change_pct : snapshot.change_pct, 1);
      if (shares[i] > 0.0f)
      {
        char worth[20];
        format_decimal_2(worth, sizeof(worth), shares[i] * value, 0);
        snprintf(holding, sizeof(holding), "$%s", worth);
        portfolio += shares[i] * value;
      }
    }

    hc05_printf("%2u  %-8s %8s %8s%%  %s\r\n", (unsigned)i, symbols[i], price,
                change, holding);

    if (above[i] > 0.0f || below[i] > 0.0f)
    {
      char high[20];
      char low[20];
      format_decimal_2(high, sizeof(high), above[i], 0);
      format_decimal_2(low, sizeof(low), below[i], 0);
      hc05_printf("     alert:%s%s%s%s\r\n",
                  above[i] > 0.0f ? " above " : "",
                  above[i] > 0.0f ? high : "",
                  below[i] > 0.0f ? " below " : "",
                  below[i] > 0.0f ? low : "");
    }
  }

  if (portfolio > 0.0f)
  {
    char total[24];
    format_decimal_2(total, sizeof(total), portfolio, 0);
    hc05_printf("Portfolio $%s\r\n", total);
  }
}

static void print_status(void)
{
  net_link_status_t link;
  net_link_get_status(&link);

  const char *session = "unknown";
  stock_snapshot_t snapshot;
  if (stock_data_get(&snapshot))
  {
    switch (snapshot.session)
    {
      case STOCK_SESSION_PRE:     session = "pre-market";  break;
      case STOCK_SESSION_REGULAR: session = "market open"; break;
      case STOCK_SESSION_POST:    session = "after hours"; break;
      case STOCK_SESSION_CLOSED:  session = "closed";      break;
      default: break;
    }
  }

  hc05_printf("\r\nLink      %s (%s)\r\n"
              "IP        %s\r\n"
              "Web admin http://%s/\r\n"
              "Session   %s\r\n"
              "Refresh   %lus\r\n"
              "Uptime    %lus\r\n"
              "Log       %s (dropped %lu bytes)\r\n",
              net_link_name(link.kind), link.bound ? "bound" : "no lease",
              link.bound ? link.ip : "-",
              link.bound ? link.ip : "-",
              session,
              (unsigned long)settings_get_refresh_seconds(),
              (unsigned long)(HAL_GetTick() / 1000U),
              hc05_get_log_mirror() ? "on" : "off",
              (unsigned long)hc05_dropped());
}

static void handle_alert(char **cursor)
{
  const char *index_text = next_token(cursor);
  const char *side = next_token(cursor);
  if (index_text == NULL || side == NULL)
  {
    hc05_printf("usage: alert <N> above|below <PRICE>, or alert <N> off\r\n");
    return;
  }

  size_t index = (size_t)strtoul(index_text, NULL, 10);
  float above[APP_MAX_SYMBOLS];
  float below[APP_MAX_SYMBOLS];
  settings_get_alerts(above, below);
  if (index >= APP_MAX_SYMBOLS)
  {
    hc05_printf("no symbol %u\r\n", (unsigned)index);
    return;
  }

  float new_above = above[index];
  float new_below = below[index];

  if (word_is(side, "off"))
  {
    new_above = 0.0f;
    new_below = 0.0f;
  }
  else
  {
    const char *price_text = next_token(cursor);
    if (price_text == NULL)
    {
      hc05_printf("usage: alert <N> above|below <PRICE>\r\n");
      return;
    }
    float price = strtof(price_text, NULL);
    if (word_is(side, "above")) new_above = price;
    else if (word_is(side, "below")) new_below = price;
    else
    {
      hc05_printf("expected 'above', 'below' or 'off'\r\n");
      return;
    }
  }

  if (settings_set_alert(index, new_above, new_below))
  {
    hc05_printf("ok: alerts updated\r\n");
  }
  else
  {
    hc05_printf("failed: no symbol %u, or price out of range\r\n",
                (unsigned)index);
  }
}

static void handle_command(char *line)
{
  char *cursor = line;
  const char *command = next_token(&cursor);
  if (command == NULL) return;    /* bare Enter */

  if (word_is(command, "help") || word_is(command, "?"))
  {
    print_help();
  }
  else if (word_is(command, "list") || word_is(command, "ls"))
  {
    print_list();
  }
  else if (word_is(command, "status"))
  {
    print_status();
  }
  else if (word_is(command, "add"))
  {
    const char *symbol = next_token(&cursor);
    if (symbol == NULL) hc05_printf("usage: add <SYMBOL>\r\n");
    else if (settings_add_symbol(symbol)) hc05_printf("ok: added\r\n");
    else hc05_printf("failed: invalid, duplicate, or list full\r\n");
  }
  else if (word_is(command, "del") || word_is(command, "delete"))
  {
    const char *index = next_token(&cursor);
    if (index == NULL) hc05_printf("usage: del <N>\r\n");
    else if (settings_delete_symbol((size_t)strtoul(index, NULL, 10)))
    {
      hc05_printf("ok: deleted\r\n");
    }
    else hc05_printf("failed: bad index, or last symbol\r\n");
  }
  else if (word_is(command, "shares"))
  {
    const char *index = next_token(&cursor);
    const char *quantity = next_token(&cursor);
    if (index == NULL || quantity == NULL)
    {
      hc05_printf("usage: shares <N> <QTY>\r\n");
    }
    else if (settings_set_shares((size_t)strtoul(index, NULL, 10),
                                 strtof(quantity, NULL)))
    {
      hc05_printf("ok: shares updated\r\n");
    }
    else hc05_printf("failed: bad index or quantity\r\n");
  }
  else if (word_is(command, "refresh"))
  {
    const char *seconds = next_token(&cursor);
    if (seconds == NULL) hc05_printf("usage: refresh <SECONDS>\r\n");
    else if (settings_set_refresh_seconds(
                 (uint32_t)strtoul(seconds, NULL, 10)))
    {
      hc05_printf("ok: refresh updated\r\n");
    }
    else hc05_printf("failed: must be 15-3600\r\n");
  }
  else if (word_is(command, "alert"))
  {
    handle_alert(&cursor);
  }
  else if (word_is(command, "log"))
  {
    const char *state = next_token(&cursor);
    if (state != NULL && word_is(state, "on"))
    {
      hc05_set_log_mirror(true);
      hc05_printf("ok: log mirror on\r\n");
    }
    else if (state != NULL && word_is(state, "off"))
    {
      hc05_set_log_mirror(false);
      hc05_printf("ok: log mirror off\r\n");
    }
    else hc05_printf("usage: log on|off\r\n");
  }
  else
  {
    hc05_printf("unknown command '%s' - try 'help'\r\n", command);
  }
}

/* ---- Alerts --------------------------------------------------------------- */

static void check_alerts(void)
{
  /* Any watchlist edit shifts the index-aligned thresholds, so re-arm
   * everything rather than fire against stale state. */
  uint32_t generation = settings_generation();
  if (generation != alerts_seen_generation)
  {
    alerts_seen_generation = generation;
    for (size_t i = 0; i < APP_MAX_SYMBOLS; ++i)
    {
      above_armed[i] = true;
      below_armed[i] = true;
    }
  }

  size_t count = settings_get_symbols(symbols);
  float above[APP_MAX_SYMBOLS];
  float below[APP_MAX_SYMBOLS];
  settings_get_alerts(above, below);

  for (size_t i = 0; i < count; ++i)
  {
    if (above[i] <= 0.0f && below[i] <= 0.0f) continue;

    stock_snapshot_t snapshot;
    if (!find_snapshot(symbols[i], &snapshot) || !snapshot.fresh) continue;
    float price = effective_price(&snapshot);

    char price_text[20];
    char threshold_text[20];
    format_decimal_2(price_text, sizeof(price_text), price, 0);

    if (above[i] > 0.0f)
    {
      if (price >= above[i] && above_armed[i])
      {
        above_armed[i] = false;
        format_decimal_2(threshold_text, sizeof(threshold_text), above[i], 0);
        hc05_printf("\r\n*** ALERT %s $%s rose above $%s ***\r\n",
                    symbols[i], price_text, threshold_text);
        printf("[alert] %s above threshold\r\n", symbols[i]);
      }
      else if (price < above[i])
      {
        above_armed[i] = true;
      }
    }

    if (below[i] > 0.0f)
    {
      if (price <= below[i] && below_armed[i])
      {
        below_armed[i] = false;
        format_decimal_2(threshold_text, sizeof(threshold_text), below[i], 0);
        hc05_printf("\r\n*** ALERT %s $%s fell below $%s ***\r\n",
                    symbols[i], price_text, threshold_text);
        printf("[alert] %s below threshold\r\n", symbols[i]);
      }
      else if (price > below[i])
      {
        below_armed[i] = true;
      }
    }
  }
}

/* ---- Task ----------------------------------------------------------------- */

void StartBtConsoleTask(void const *argument)
{
  (void)argument;

  hc05_init();

  for (size_t i = 0; i < APP_MAX_SYMBOLS; ++i)
  {
    above_armed[i] = true;
    below_armed[i] = true;
  }
  alerts_seen_generation = settings_generation();

  hc05_printf("\r\nROZAKOS INDUSTRIES stock ticker\r\n"
              "Bluetooth console ready - type 'help'\r\n");

  uint32_t last_alert_check = osKernelSysTick();

  for (;;)
  {
    char line[96];
    while (hc05_read_line(line, sizeof(line)))
    {
      handle_command(line);
    }

    if ((osKernelSysTick() - last_alert_check) >= ALERT_POLL_MS)
    {
      last_alert_check = osKernelSysTick();
      check_alerts();
    }

    osDelay(100);
  }
}

void bt_console_start(void)
{
  /* 2048 words. Command handling itself is cheap (one snapshot at a time),
   * but every settings_set_* call nests settings_save -> flash_save +
   * FatFs (FIL is ~550 bytes) + printf frames - the same path that once
   * overflowed the web task at 2048 words. Priority Low: it must never
   * compete with the UI or the TLS client. */
  osThreadDef(btTask, StartBtConsoleTask, osPriorityLow, 0, 2048);
  if (osThreadCreate(osThread(btTask), NULL) == NULL)
  {
    printf("[bt] failed to start console task\r\n");
  }
}
