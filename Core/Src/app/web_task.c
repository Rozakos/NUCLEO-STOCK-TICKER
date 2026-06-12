#include "app/web_task.h"
#include "app/format.h"
#include "app/settings.h"
#include "app/stock_data.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lwip/inet.h"
#include "lwip/netif.h"
#include "lwip/sockets.h"

#define WEB_BUFFER_ADDR ((char *)0xC0068000U)
#define WEB_BUFFER_SIZE (20U * 1024U)

extern struct netif gnetif;

static size_t append(char *buffer, size_t used, const char *text)
{
  size_t length = strlen(text);
  if (used + length < WEB_BUFFER_SIZE)
  {
    memcpy(buffer + used, text, length);
    return used + length;
  }
  return used;
}

static size_t append_symbols_page(char *buffer)
{
  char symbols[APP_MAX_SYMBOLS][APP_SYMBOL_LENGTH];
  stock_snapshot_t stocks[APP_MAX_SYMBOLS];
  float shares[APP_MAX_SYMBOLS];
  size_t symbol_count = settings_get_symbols(symbols);
  size_t stock_count = stock_data_get_all(stocks);
  settings_get_shares(shares);
  size_t used = 0;

  used = append(buffer, used,
    "<!doctype html><html><head><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Rozakos Industries - Stock Ticker</title><style>"
    ":root{--bg:#0d1117;--card:#161b22;--border:#30363d;--text:#e6edf3;--muted:#8b949e;--red:#c42525;--green:#4ade80}"
    "*{box-sizing:border-box}body{font-family:system-ui,sans-serif;max-width:540px;margin:auto;padding:0 16px 24px;background:var(--bg);color:var(--text)}"
    "header{background:#110808;border-bottom:1px solid #3d1c1c;margin:0 -16px 18px;padding:16px}h1{font-size:20px;margin:0}h2{font-size:11px;margin:22px 0 8px;color:var(--muted);text-transform:uppercase;letter-spacing:1.5px}"
    ".card,table{width:100%;background:var(--card);border:1px solid var(--border);border-radius:8px}.card{padding:12px;margin:7px 0;display:flex;justify-content:space-between}"
    "table{border-collapse:separate;border-spacing:0;overflow:hidden}td{padding:9px 12px;border-top:1px solid var(--border)}tr:first-child td{border-top:0}"
    ".muted{color:var(--muted)}input,button{padding:9px;border-radius:6px;border:1px solid var(--border);background:#0d1117;color:var(--text);font-size:15px}"
    "button{background:var(--red);font-weight:bold}.add{display:flex;gap:8px}.add input{flex:1;min-width:0}.del{padding:5px 10px}.right{text-align:right}.up{color:var(--green)}.down{color:#f87171}"
    "</style></head><body><header><h1>ROZAKOS INDUSTRIES</h1><div class=muted>STM32 Stock Ticker Web Admin</div></header>"
    "<h2>Live Markets</h2>");

  for (size_t i = 0; i < symbol_count; ++i)
  {
    const stock_snapshot_t *found = NULL;
    for (size_t j = 0; j < stock_count; ++j)
    {
      if (strcmp(symbols[i], stocks[j].symbol) == 0) found = &stocks[j];
    }
    char row[360];
    if (found != NULL && found->fresh)
    {
      char price[20], change[20], holding[64] = "";
      format_decimal_2(price, sizeof(price), found->last, 0);
      format_decimal_2(change, sizeof(change), found->change_pct, 1);
      if (shares[i] > 0.0f)
      {
        char value[20];
        format_decimal_2(value, sizeof(value), shares[i] * found->last, 0);
        snprintf(holding, sizeof(holding),
                 "<span class=muted>$%s</span> &nbsp; ", value);
      }
      snprintf(row, sizeof(row),
               "<div class=card><strong>%s</strong><span>%s$%s &nbsp; <span class=%s>%s%%</span></span></div>",
               symbols[i], holding, price,
               found->change_pct >= 0.0f ? "up" : "down", change);
    }
    else
    {
      snprintf(row, sizeof(row),
               "<div class=card><strong>%s</strong><span class=muted>waiting...</span></div>",
               symbols[i]);
    }
    used = append(buffer, used, row);
  }

  used = append(buffer, used,
                "<h2>Symbols &amp; Shares Owned</h2><table>");
  for (size_t i = 0; i < symbol_count; ++i)
  {
    char row[520];
    char quantity[20];
    format_decimal_2(quantity, sizeof(quantity), shares[i], 0);
    snprintf(row, sizeof(row),
             "<tr><td><strong>%s</strong></td><td>"
             "<form class=add method=POST action=/shares>"
             "<input type=hidden name=i value=%u>"
             "<input name=qty type=number step=any min=0 value=%s style='width:110px;flex:none'>"
             "<button class=del type=submit>Set</button></form></td>"
             "<td class=right>"
             "<form method=POST action=/delete><input type=hidden name=i value=%u>"
             "<button class=del type=submit>&times;</button></form></td></tr>",
             symbols[i], (unsigned)i, quantity, (unsigned)i);
    used = append(buffer, used, row);
  }
  used = append(buffer, used, "</table>");

  char form[2300];
  snprintf(form, sizeof(form),
    "<datalist id=symbols><option value=AAPL><option value=MSFT><option value=GOOGL>"
    "<option value=AMZN><option value=META><option value=TSLA><option value=NVDA>"
    "<option value=AMD><option value=INTC><option value=TSM><option value=HPE>"
    "<option value=NOK><option value=SPY><option value=QQQ></datalist>"
    "<h2>Add Symbol</h2><form class=add method=POST action=/add>"
    "<input name=symbol placeholder=AMD maxlength=11 required list=symbols>"
    "<button type=submit>Add</button></form>"
    "<h2>Settings</h2><form class=add method=POST action=/settings>"
    "<input name=refresh_s type=number min=15 max=3600 value=%lu required>"
    "<button type=submit>Save refresh</button></form>"
    "<p class=muted>Seconds between quote refreshes. Up to %u symbols. "
    "Settings apply immediately but reset to firmware defaults after reboot.</p>"
    "</body></html>",
    (unsigned long)settings_get_refresh_seconds(), APP_MAX_SYMBOLS);
  used = append(buffer, used, form);
  buffer[used] = '\0';
  return used;
}

static void url_decode(char *value)
{
  char *out = value;
  for (char *in = value; *in != '\0'; ++in)
  {
    if (*in == '+')
    {
      *out++ = ' ';
    }
    else if (*in == '%' && in[1] != '\0' && in[2] != '\0')
    {
      unsigned hex;
      if (sscanf(in + 1, "%2x", &hex) == 1)
      {
        *out++ = (char)hex;
        in += 2;
      }
    }
    else
    {
      *out++ = *in;
    }
  }
  *out = '\0';
}

static void send_all(int client, const char *data, size_t length)
{
  size_t sent = 0;
  while (sent < length)
  {
    int result = send(client, data + sent, length - sent, 0);
    if (result <= 0) break;
    sent += (size_t)result;
  }
}

static int receive_request(int client, char *request, size_t capacity)
{
  size_t used = 0;
  size_t expected = 0;
  while (used < capacity - 1U)
  {
    int received = recv(client, request + used, capacity - 1U - used, 0);
    if (received <= 0) break;
    used += (size_t)received;
    request[used] = '\0';

    char *body = strstr(request, "\r\n\r\n");
    if (body != NULL)
    {
      if (expected == 0U)
      {
        char *length = strstr(request, "Content-Length:");
        expected = (size_t)(body + 4 - request);
        if (length != NULL)
        {
          expected += (size_t)strtoul(length + 15, NULL, 10);
        }
      }
      if (used >= expected) break;
    }
  }
  request[used] = '\0';
  return (int)used;
}

static char *form_value(char *request, const char *name)
{
  char *body = strstr(request, "\r\n\r\n");
  if (body == NULL) return NULL;
  body += 4;

  size_t name_length = strlen(name);
  for (char *field = body; field != NULL && *field != '\0';)
  {
    if (strncmp(field, name, name_length) == 0 && field[name_length] == '=')
    {
      char *value = field + name_length + 1U;
      char *end = strchr(value, '&');
      if (end != NULL) *end = '\0';
      url_decode(value);
      return value;
    }
    field = strchr(field, '&');
    if (field != NULL) ++field;
  }
  return NULL;
}

static void handle_client(int client)
{
  char request[1536];
  int received = receive_request(client, request, sizeof(request));
  if (received <= 0) return;

  if (strncmp(request, "POST /add ", 10) == 0)
  {
    char *symbol = form_value(request, "symbol");
    if (symbol != NULL) settings_add_symbol(symbol);
  }
  else if (strncmp(request, "POST /delete ", 13) == 0)
  {
    char *index = form_value(request, "i");
    if (index != NULL) settings_delete_symbol((size_t)strtoul(index, NULL, 10));
  }
  else if (strncmp(request, "POST /shares ", 13) == 0)
  {
    /* Extract qty BEFORE i: form_value() null-terminates the value at its
     * trailing '&', which would cut the body off after "i=N" and hide the
     * qty field. qty is the last field, so reading it first mutates nothing. */
    char *quantity = form_value(request, "qty");
    char *index = form_value(request, "i");
    if (quantity != NULL && index != NULL)
    {
      settings_set_shares((size_t)strtoul(index, NULL, 10),
                          strtof(quantity, NULL));
    }
  }
  else if (strncmp(request, "POST /settings ", 15) == 0)
  {
    char *refresh = form_value(request, "refresh_s");
    if (refresh != NULL)
    {
      settings_set_refresh_seconds((uint32_t)strtoul(refresh, NULL, 10));
    }
  }

  size_t body_length = append_symbols_page(WEB_BUFFER_ADDR);
  char header[180];
  int header_length = snprintf(header, sizeof(header),
      "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: %u\r\n"
      "Connection: close\r\nCache-Control: no-store\r\n\r\n",
      (unsigned)body_length);
  send_all(client, header, (size_t)header_length);
  send_all(client, WEB_BUFFER_ADDR, body_length);
}

void StartWebTask(void const *argument)
{
  (void)argument;
  settings_storage_load();
  while (ip4_addr_isany_val(*netif_ip4_addr(&gnetif))) osDelay(250);

  int server = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in address = { 0 };
  address.sin_family = AF_INET;
  address.sin_port = lwip_htons(80);
  address.sin_addr.s_addr = PP_HTONL(INADDR_ANY);
  if (server < 0 || bind(server, (struct sockaddr *)&address, sizeof(address)) != 0 ||
      listen(server, 2) != 0)
  {
    printf("[web] failed to listen on port 80\r\n");
    osThreadTerminate(NULL);
  }

  printf("[web] admin ready: http://%s/\r\n", ip4addr_ntoa(netif_ip4_addr(&gnetif)));
  for (;;)
  {
    int client = accept(server, NULL, NULL);
    if (client >= 0)
    {
      handle_client(client);
      closesocket(client);
    }
  }
}
