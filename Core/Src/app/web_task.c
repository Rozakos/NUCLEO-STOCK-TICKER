#include "app/web_task.h"
#include "app/format.h"
#include "app/settings.h"
#include "app/stock_data.h"

#include <stdio.h>
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
  size_t symbol_count = settings_get_symbols(symbols);
  size_t stock_count = stock_data_get_all(stocks);
  size_t used = 0;

  used = append(buffer, used,
    "<!doctype html><html><head><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>NUCLEO Stock Ticker</title><style>"
    ":root{--bg:#0b0f17;--card:#16202d;--border:#303b4a;--text:#e7eef7;--muted:#8a98ad;--red:#c42525;--green:#4ade80}"
    "*{box-sizing:border-box}body{font-family:system-ui,sans-serif;max-width:620px;margin:auto;padding:20px;background:var(--bg);color:var(--text)}"
    "header{border-bottom:2px solid var(--red);margin-bottom:20px}h1{font-size:22px}h2{font-size:13px;color:var(--muted);text-transform:uppercase;letter-spacing:1.5px}"
    ".card{background:var(--card);border:1px solid var(--border);border-radius:10px;padding:14px;margin:8px 0;display:flex;justify-content:space-between}"
    ".muted{color:var(--muted)}input,button{padding:11px;border-radius:7px;border:1px solid var(--border);background:#0d131d;color:var(--text);font-size:16px}"
    "input{width:100%}button{background:var(--red);font-weight:bold;margin-top:10px;width:100%}.up{color:var(--green)}"
    "</style></head><body><header><h1>ROZAKOS INDUSTRIES</h1><p class=muted>STM32 Stock Ticker Web Admin</p></header>"
    "<h2>Live Markets</h2>");

  for (size_t i = 0; i < symbol_count; ++i)
  {
    const stock_snapshot_t *found = NULL;
    for (size_t j = 0; j < stock_count; ++j)
    {
      if (strcmp(symbols[i], stocks[j].symbol) == 0) found = &stocks[j];
    }
    char row[220];
    if (found != NULL && found->fresh)
    {
      char price[20], change[20];
      format_decimal_2(price, sizeof(price), found->last, 0);
      format_decimal_2(change, sizeof(change), found->change_pct, 1);
      snprintf(row, sizeof(row),
               "<div class=card><strong>%s</strong><span>$%s &nbsp; <span class=up>%s%%</span></span></div>",
               symbols[i], price, change);
    }
    else
    {
      snprintf(row, sizeof(row),
               "<div class=card><strong>%s</strong><span class=muted>waiting...</span></div>",
               symbols[i]);
    }
    used = append(buffer, used, row);
  }

  char csv[APP_MAX_SYMBOLS * APP_SYMBOL_LENGTH] = { 0 };
  for (size_t i = 0; i < symbol_count; ++i)
  {
    if (i > 0U) strncat(csv, ",", sizeof(csv) - strlen(csv) - 1U);
    strncat(csv, symbols[i], sizeof(csv) - strlen(csv) - 1U);
  }
  char form[700];
  snprintf(form, sizeof(form),
    "<h2>Symbols</h2><form method=POST action=/symbols>"
    "<input name=symbols value='%s' maxlength=95>"
    "<p class=muted>Comma-separated, up to %u symbols. Changes apply immediately.</p>"
    "<button type=submit>Save symbols</button></form></body></html>",
    csv, APP_MAX_SYMBOLS);
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

static void handle_client(int client)
{
  char request[1024];
  int received = recv(client, request, sizeof(request) - 1U, 0);
  if (received <= 0) return;
  request[received] = '\0';

  if (strncmp(request, "POST /symbols ", 14) == 0)
  {
    char *body = strstr(request, "\r\n\r\n");
    if (body != NULL && strncmp(body + 4, "symbols=", 8) == 0)
    {
      body += 12;
      char *end = strpbrk(body, "\r\n&");
      if (end != NULL) *end = '\0';
      url_decode(body);
      settings_set_symbols_csv(body);
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
