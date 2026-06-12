#include "app/stock_api.h"
#include "app/config.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "FreeRTOS.h"
#include "cJSON.h"
#include "lwip/api.h"
#include "lwip/sockets.h"
#include "main.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/memory_buffer_alloc.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"

#include "app/tls_ca_cert_data.h"

#define TLS_HEAP_ADDR       ((unsigned char *)0xC0040000U)
#define TLS_HEAP_SIZE       (128U * 1024U)
#define HTTP_RESPONSE_ADDR  ((char *)0xC0060000U)
#define HTTP_RESPONSE_SIZE  (24U * 1024U)

static bool api_initialized;
static bool ca_initialized;
static mbedtls_x509_crt ca_cert;

static void *json_malloc(size_t size)
{
  return pvPortMalloc(size);
}

static void json_free(void *ptr)
{
  vPortFree(ptr);
}

static int hardware_entropy(void *context, unsigned char *output, size_t len,
                            size_t *written)
{
  (void)context;

  __HAL_RCC_RNG_CLK_ENABLE();
  RNG->CR |= RNG_CR_RNGEN;

  for (size_t offset = 0; offset < len;)
  {
    uint32_t deadline = HAL_GetTick() + 100U;
    while ((RNG->SR & RNG_SR_DRDY) == 0U)
    {
      if ((int32_t)(HAL_GetTick() - deadline) >= 0)
      {
        return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
      }
    }

    uint32_t value = RNG->DR;
    size_t count = len - offset;
    if (count > sizeof(value))
    {
      count = sizeof(value);
    }
    memcpy(output + offset, &value, count);
    offset += count;
  }

  *written = len;
  return 0;
}

static int socket_send(void *context, const unsigned char *buffer, size_t len)
{
  int socket_fd = *(int *)context;
  int result = send(socket_fd, buffer, len, 0);
  return result >= 0 ? result : MBEDTLS_ERR_NET_SEND_FAILED;
}

static int socket_recv(void *context, unsigned char *buffer, size_t len)
{
  int socket_fd = *(int *)context;
  int result = recv(socket_fd, buffer, len, 0);
  if (result >= 0)
  {
    return result;
  }
  if (errno == EWOULDBLOCK || errno == EAGAIN)
  {
    return MBEDTLS_ERR_SSL_WANT_READ;
  }
  return MBEDTLS_ERR_NET_RECV_FAILED;
}

static void format_tls_error(char *error, size_t error_size, const char *stage,
                             int code)
{
  char detail[80];
  mbedtls_strerror(code, detail, sizeof(detail));
  snprintf(error, error_size, "%s: %s (-0x%04X)", stage, detail,
           (unsigned)-code);
}

static int connect_socket(const char *host, uint16_t port, char *error,
                          size_t error_size)
{
  ip_addr_t address;
  if (netconn_gethostbyname(host, &address) != ERR_OK)
  {
    snprintf(error, error_size, "DNS failed");
    return -1;
  }

  int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (socket_fd < 0)
  {
    snprintf(error, error_size, "socket failed");
    return -1;
  }

  struct timeval timeout = { .tv_sec = 12, .tv_usec = 0 };
  setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

  struct sockaddr_in server = { 0 };
  server.sin_family = AF_INET;
  server.sin_port = lwip_htons(port);
  server.sin_addr.s_addr = ip_addr_get_ip4_u32(&address);
  if (connect(socket_fd, (struct sockaddr *)&server, sizeof(server)) != 0)
  {
    snprintf(error, error_size, "TCP connect failed");
    closesocket(socket_fd);
    return -1;
  }

  return socket_fd;
}

/* ---- Persistent TLS connection ------------------------------------------
 * Only the net task fetches, so one TLS connection is kept open across
 * requests (HTTP/1.1 keep-alive): a button tap normally costs a single
 * round-trip instead of a multi-second handshake. When the server or an
 * idle timeout drops the socket, the next request reconnects and resumes
 * the saved TLS session (abbreviated handshake - no key exchange or chain
 * verification). The probed API (Cloudflare) always returns Content-Length
 * for identity encoding, so chunked responses are rejected as errors. */
static bool tls_stack_ready;
static mbedtls_entropy_context tls_entropy;
static mbedtls_ctr_drbg_context tls_drbg;
static mbedtls_ssl_config tls_config;
static mbedtls_ssl_context tls_ssl;
static mbedtls_ssl_session tls_session;
static bool tls_session_valid;
static int tls_socket = -1;

#define HTTP_RESPONSE_TIMEOUT_MS 20000U

static void tls_disconnect(void)
{
  if (tls_socket >= 0)
  {
    mbedtls_ssl_close_notify(&tls_ssl);
    closesocket(tls_socket);
    tls_socket = -1;
  }
}

/* One-time setup of the contexts that persist across connections. */
static int tls_stack_init(char *error, size_t error_size)
{
  if (tls_stack_ready) return 0;

  mbedtls_entropy_init(&tls_entropy);
  mbedtls_ctr_drbg_init(&tls_drbg);
  mbedtls_ssl_init(&tls_ssl);
  mbedtls_ssl_config_init(&tls_config);
  mbedtls_ssl_session_init(&tls_session);

  int ret = mbedtls_entropy_add_source(&tls_entropy, hardware_entropy, NULL,
                                       32, MBEDTLS_ENTROPY_SOURCE_STRONG);
  if (ret != 0)
  {
    format_tls_error(error, error_size, "entropy", ret);
    return -1;
  }

  static const unsigned char personalization[] = "nucleo-stock-ticker";
  ret = mbedtls_ctr_drbg_seed(&tls_drbg, mbedtls_entropy_func, &tls_entropy,
                              personalization, sizeof(personalization) - 1U);
  if (ret != 0)
  {
    format_tls_error(error, error_size, "DRBG", ret);
    return -1;
  }

  ret = mbedtls_ssl_config_defaults(&tls_config, MBEDTLS_SSL_IS_CLIENT,
                                    MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT);
  if (ret != 0)
  {
    format_tls_error(error, error_size, "TLS config", ret);
    return -1;
  }
#if TLS_INSECURE_SKIP_VERIFY
  mbedtls_ssl_conf_authmode(&tls_config, MBEDTLS_SSL_VERIFY_NONE);
#else
  if (!ca_initialized)
  {
    snprintf(error, error_size, "pinned CA unavailable");
    return -1;
  }
  mbedtls_ssl_conf_ca_chain(&tls_config, &ca_cert, NULL);
  mbedtls_ssl_conf_authmode(&tls_config, MBEDTLS_SSL_VERIFY_REQUIRED);
#endif
  mbedtls_ssl_conf_rng(&tls_config, mbedtls_ctr_drbg_random, &tls_drbg);
  mbedtls_ssl_conf_session_tickets(&tls_config,
                                   MBEDTLS_SSL_SESSION_TICKETS_ENABLED);

  ret = mbedtls_ssl_setup(&tls_ssl, &tls_config);
  if (ret != 0)
  {
    format_tls_error(error, error_size, "TLS setup", ret);
    return -1;
  }
  ret = mbedtls_ssl_set_hostname(&tls_ssl, STOCK_API_HOST);
  if (ret != 0)
  {
    format_tls_error(error, error_size, "TLS hostname", ret);
    return -1;
  }

  tls_stack_ready = true;
  return 0;
}

static int tls_connect(char *error, size_t error_size)
{
  if (tls_socket >= 0) return 0;

  int ret = mbedtls_ssl_session_reset(&tls_ssl);
  if (ret != 0)
  {
    format_tls_error(error, error_size, "TLS reset", ret);
    return -1;
  }
  if (tls_session_valid &&
      mbedtls_ssl_set_session(&tls_ssl, &tls_session) != 0)
  {
    tls_session_valid = false;   /* resumption is best-effort */
  }

  tls_socket = connect_socket(STOCK_API_HOST, STOCK_API_PORT, error,
                              error_size);
  if (tls_socket < 0) return -1;
  mbedtls_ssl_set_bio(&tls_ssl, &tls_socket, socket_send, socket_recv, NULL);

  uint32_t handshake_start = HAL_GetTick();
  while ((ret = mbedtls_ssl_handshake(&tls_ssl)) != 0)
  {
    if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE)
    {
      format_tls_error(error, error_size, "TLS handshake", ret);
      closesocket(tls_socket);
      tls_socket = -1;
      return -1;
    }
  }
  printf("[tls] handshake %lums\r\n",
         (unsigned long)(HAL_GetTick() - handshake_start));

  /* Save the (possibly refreshed) session for the next reconnect. */
  mbedtls_ssl_session_free(&tls_session);
  mbedtls_ssl_session_init(&tls_session);
  tls_session_valid = (mbedtls_ssl_get_session(&tls_ssl, &tls_session) == 0);
  return 0;
}

/* Case-insensitive lookup in the NUL-terminated response header block;
 * returns the start of the value or NULL. */
static const char *find_header(const char *headers, const char *name)
{
  size_t name_len = strlen(name);
  for (const char *line = strstr(headers, "\r\n"); line != NULL;
       line = strstr(line + 2, "\r\n"))
  {
    const char *field = line + 2;
    size_t i = 0;
    while (i < name_len &&
           tolower((unsigned char)field[i]) ==
               tolower((unsigned char)name[i]))
    {
      ++i;
    }
    if (i == name_len && field[i] == ':')
    {
      const char *value = field + name_len + 1U;
      while (*value == ' ') ++value;
      return value;
    }
  }
  return NULL;
}

/* One request over the open connection.
 * Returns 0 on success, -1 on a transport failure (worth a reconnect and
 * retry), -2 on an HTTP/protocol error (final). */
static int do_https_request(const char *path, char **body, size_t *body_len,
                            char *error, size_t error_size)
{
  char request[768];
  int request_len = snprintf(request, sizeof(request),
      "GET %s HTTP/1.1\r\n"
      "Host: %s\r\n"
      "User-Agent: NUCLEO-Stock-Ticker/1.0 (STM32F746)\r\n"
      "Accept: */*\r\n"
      "Accept-Encoding: identity\r\n"
      "Authorization: Bearer %s\r\n"
      "Connection: keep-alive\r\n\r\n",
      path, STOCK_API_HOST, STOCK_API_TOKEN);
  if (request_len <= 0 || (size_t)request_len >= sizeof(request))
  {
    snprintf(error, error_size, "request too large");
    return -2;
  }

  int ret;
  size_t sent = 0;
  while (sent < (size_t)request_len)
  {
    ret = mbedtls_ssl_write(&tls_ssl, (unsigned char *)request + sent,
                            (size_t)request_len - sent);
    if (ret > 0)
    {
      sent += (size_t)ret;
    }
    else if (ret != MBEDTLS_ERR_SSL_WANT_READ &&
             ret != MBEDTLS_ERR_SSL_WANT_WRITE)
    {
      format_tls_error(error, error_size, "HTTPS write", ret);
      return -1;
    }
  }

  /* Read until the header/body split is in the buffer. */
  uint32_t deadline = HAL_GetTick() + HTTP_RESPONSE_TIMEOUT_MS;
  size_t used = 0;
  char *header_end = NULL;
  while (header_end == NULL)
  {
    if (used >= HTTP_RESPONSE_SIZE - 1U)
    {
      snprintf(error, error_size, "response too large");
      return -2;
    }
    if ((int32_t)(HAL_GetTick() - deadline) >= 0)
    {
      snprintf(error, error_size, "response timeout");
      return -1;
    }
    ret = mbedtls_ssl_read(&tls_ssl, (unsigned char *)HTTP_RESPONSE_ADDR + used,
                           HTTP_RESPONSE_SIZE - 1U - used);
    if (ret > 0)
    {
      used += (size_t)ret;
      HTTP_RESPONSE_ADDR[used] = '\0';
      header_end = strstr(HTTP_RESPONSE_ADDR, "\r\n\r\n");
      continue;
    }
    if (ret == 0 || ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
    {
      snprintf(error, error_size, "connection closed");
      return -1;
    }
    if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
    {
      continue;
    }
    format_tls_error(error, error_size, "HTTPS read", ret);
    return -1;
  }
  *header_end = '\0';   /* terminate the header block for parsing */
  char *body_start = header_end + 4;

  if (strncmp(HTTP_RESPONSE_ADDR, "HTTP/1.1 200 ", 13) != 0 &&
      strncmp(HTTP_RESPONSE_ADDR, "HTTP/1.0 200 ", 13) != 0)
  {
    char *line_end = strstr(HTTP_RESPONSE_ADDR, "\r\n");
    if (line_end != NULL) *line_end = '\0';
    snprintf(error, error_size, "%.40s", HTTP_RESPONSE_ADDR);
    return -2;
  }

  const char *transfer = find_header(HTTP_RESPONSE_ADDR, "transfer-encoding");
  if (transfer != NULL && strncasecmp(transfer, "identity", 8) != 0)
  {
    snprintf(error, error_size, "chunked response unsupported");
    return -2;
  }

  const char *length_value = find_header(HTTP_RESPONSE_ADDR, "content-length");
  size_t header_size = (size_t)(body_start - HTTP_RESPONSE_ADDR);
  size_t have = used - header_size;
  size_t content_length;
  bool reusable = true;
  if (length_value != NULL)
  {
    content_length = (size_t)strtoul(length_value, NULL, 10);
    if (content_length > HTTP_RESPONSE_SIZE - 1U - header_size)
    {
      snprintf(error, error_size, "response too large");
      return -2;
    }
    while (have < content_length)
    {
      if ((int32_t)(HAL_GetTick() - deadline) >= 0)
      {
        snprintf(error, error_size, "response timeout");
        return -1;
      }
      ret = mbedtls_ssl_read(&tls_ssl,
                             (unsigned char *)HTTP_RESPONSE_ADDR + used,
                             HTTP_RESPONSE_SIZE - 1U - used);
      if (ret > 0)
      {
        used += (size_t)ret;
        have += (size_t)ret;
        continue;
      }
      if (ret == 0 || ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
      {
        snprintf(error, error_size, "connection closed");
        return -1;
      }
      if (ret == MBEDTLS_ERR_SSL_WANT_READ ||
          ret == MBEDTLS_ERR_SSL_WANT_WRITE)
      {
        continue;
      }
      format_tls_error(error, error_size, "HTTPS read", ret);
      return -1;
    }
  }
  else
  {
    /* No length given: read to close; this connection can't be reused. */
    reusable = false;
    for (;;)
    {
      if (used >= HTTP_RESPONSE_SIZE - 1U) break;
      if ((int32_t)(HAL_GetTick() - deadline) >= 0) break;
      ret = mbedtls_ssl_read(&tls_ssl,
                             (unsigned char *)HTTP_RESPONSE_ADDR + used,
                             HTTP_RESPONSE_SIZE - 1U - used);
      if (ret > 0)
      {
        used += (size_t)ret;
        continue;
      }
      if (ret == MBEDTLS_ERR_SSL_WANT_READ ||
          ret == MBEDTLS_ERR_SSL_WANT_WRITE)
      {
        continue;
      }
      break;   /* closed (or errored) - we have what we have */
    }
    content_length = used - header_size;
  }
  HTTP_RESPONSE_ADDR[used] = '\0';

  const char *connection = find_header(HTTP_RESPONSE_ADDR, "connection");
  if (connection != NULL && strncasecmp(connection, "close", 5) == 0)
  {
    reusable = false;
  }
  if (!reusable)
  {
    tls_disconnect();
  }

  *body = body_start;
  if (body_len != NULL)
  {
    *body_len = content_length;
  }
  return 0;
}

static int https_get(const char *path, char **body, size_t *body_len,
                     char *error, size_t error_size)
{
  if (tls_stack_init(error, error_size) != 0)
  {
    return -1;
  }

  for (int attempt = 0; attempt < 2; ++attempt)
  {
    bool reusing = (tls_socket >= 0);
    if (tls_connect(error, error_size) != 0)
    {
      return -1;   /* a fresh connect failed; retrying would do the same */
    }
    int result = do_https_request(path, body, body_len, error, error_size);
    if (result == 0)
    {
      return 0;
    }
    tls_disconnect();
    /* Transport failure on a reused connection usually means the server
     * idled it out - reconnect once and retry. Anything else is final. */
    if (result != -1 || !reusing)
    {
      return -1;
    }
  }
  return -1;
}

void stock_api_init(void)
{
  if (api_initialized)
  {
    return;
  }

  mbedtls_memory_buffer_alloc_init(TLS_HEAP_ADDR, TLS_HEAP_SIZE);
  mbedtls_x509_crt_init(&ca_cert);
  int ret = mbedtls_x509_crt_parse(&ca_cert,
      (const unsigned char *)tls_ca_cert_pem, strlen(tls_ca_cert_pem) + 1U);
  if (ret == 0)
  {
    ca_initialized = true;
    printf("[tls] pinned CA loaded; verification required\r\n");
  }
  else
  {
    printf("[tls] pinned CA parse failed: -0x%04X\r\n", (unsigned)-ret);
  }
  cJSON_Hooks hooks = { .malloc_fn = json_malloc, .free_fn = json_free };
  cJSON_InitHooks(&hooks);
  api_initialized = true;
}

int stock_api_fetch_quote(const char *symbol, stock_snapshot_t *snapshot,
                          char *error, size_t error_size)
{
  char path[96];
  snprintf(path, sizeof(path), "%s/stock/%s", STOCK_API_BASE_PATH, symbol);

  char *body;
  if (https_get(path, &body, NULL, error, error_size) != 0)
  {
    return -1;
  }

  cJSON *root = cJSON_Parse(body);
  if (root == NULL)
  {
    snprintf(error, error_size, "JSON parse failed");
    return -1;
  }

  cJSON *last = cJSON_GetObjectItemCaseSensitive(root, "last");
  cJSON *change = cJSON_GetObjectItemCaseSensitive(root, "change_pct");
  cJSON *closes = cJSON_GetObjectItemCaseSensitive(root, "closes");
  if (!cJSON_IsNumber(last) || !cJSON_IsNumber(change))
  {
    cJSON_Delete(root);
    snprintf(error, error_size, "JSON missing quote");
    return -1;
  }

  memset(snapshot, 0, sizeof(*snapshot));
  snprintf(snapshot->symbol, sizeof(snapshot->symbol), "%s", symbol);
  snapshot->last = (float)last->valuedouble;
  snapshot->change_pct = (float)change->valuedouble;
  snapshot->fresh = true;
  snapshot->updated_ms = HAL_GetTick();
  snprintf(snapshot->status, sizeof(snapshot->status), "Live via HTTPS");

  if (cJSON_IsArray(closes))
  {
    cJSON *point;
    cJSON_ArrayForEach(point, closes)
    {
      if (cJSON_IsNumber(point) &&
          snapshot->close_count < STOCK_SPARKLINE_MAX_POINTS)
      {
        snapshot->closes[snapshot->close_count++] = (float)point->valuedouble;
      }
    }
  }

  cJSON_Delete(root);
  return 0;
}

int stock_api_fetch_quotes(char symbols[APP_MAX_SYMBOLS][APP_SYMBOL_LENGTH],
                           size_t count, stock_snapshot_t *snapshots,
                           size_t *fetched_count, char *error,
                           size_t error_size)
{
  char path[192];
  int used = snprintf(path, sizeof(path), "%s/stocks?symbols=",
                      STOCK_API_BASE_PATH);
  for (size_t i = 0; i < count; ++i)
  {
    used += snprintf(path + used, sizeof(path) - (size_t)used, "%s%s",
                     i == 0U ? "" : ",", symbols[i]);
    if ((size_t)used >= sizeof(path))
    {
      snprintf(error, error_size, "symbol list too long");
      return -1;
    }
  }

  char *body;
  if (https_get(path, &body, NULL, error, error_size) != 0)
  {
    return -1;
  }

  cJSON *root = cJSON_Parse(body);
  if (root == NULL)
  {
    snprintf(error, error_size, "JSON parse failed");
    return -1;
  }
  cJSON *quotes = cJSON_GetObjectItemCaseSensitive(root, "quotes");
  if (!cJSON_IsArray(quotes))
  {
    cJSON_Delete(root);
    snprintf(error, error_size, "JSON missing quotes");
    return -1;
  }

  size_t fetched = 0;
  cJSON *item;
  cJSON_ArrayForEach(item, quotes)
  {
    if (fetched >= count) break;
    cJSON *symbol = cJSON_GetObjectItemCaseSensitive(item, "symbol");
    cJSON *last = cJSON_GetObjectItemCaseSensitive(item, "last");
    cJSON *change = cJSON_GetObjectItemCaseSensitive(item, "change_pct");
    cJSON *closes = cJSON_GetObjectItemCaseSensitive(item, "closes");
    if (!cJSON_IsString(symbol) || !cJSON_IsNumber(last) ||
        !cJSON_IsNumber(change))
    {
      continue;
    }

    stock_snapshot_t *snapshot = &snapshots[fetched];
    memset(snapshot, 0, sizeof(*snapshot));
    snprintf(snapshot->symbol, sizeof(snapshot->symbol), "%.11s",
             symbol->valuestring);
    snapshot->last = (float)last->valuedouble;
    snapshot->change_pct = (float)change->valuedouble;
    snapshot->fresh = true;
    snapshot->updated_ms = HAL_GetTick();
    snprintf(snapshot->status, sizeof(snapshot->status), "Live via HTTPS");
    if (cJSON_IsArray(closes))
    {
      cJSON *point;
      cJSON_ArrayForEach(point, closes)
      {
        if (cJSON_IsNumber(point) &&
            snapshot->close_count < STOCK_SPARKLINE_MAX_POINTS)
        {
          snapshot->closes[snapshot->close_count++] =
              (float)point->valuedouble;
        }
      }
    }
    ++fetched;
  }
  cJSON_Delete(root);

  if (fetched == 0U)
  {
    snprintf(error, error_size, "no quotes in response");
    return -1;
  }
  *fetched_count = fetched;
  return 0;
}

int stock_api_fetch_history(const history_request_t *request,
                            history_snapshot_t *snapshot,
                            char *error, size_t error_size)
{
  char path[128];
  snprintf(path, sizeof(path), "%s/history/%s?range=%s&limit=%u",
           STOCK_API_BASE_PATH, request->symbol, request->range,
           STOCK_SPARKLINE_MAX_POINTS);

  char *body;
  if (https_get(path, &body, NULL, error, error_size) != 0)
  {
    return -1;
  }

  cJSON *root = cJSON_Parse(body);
  if (root == NULL)
  {
    snprintf(error, error_size, "JSON parse failed");
    return -1;
  }

  cJSON *points = cJSON_GetObjectItemCaseSensitive(root, "points");
  if (!cJSON_IsArray(points))
  {
    cJSON_Delete(root);
    snprintf(error, error_size, "JSON missing history");
    return -1;
  }

  memset(snapshot, 0, sizeof(*snapshot));
  snprintf(snapshot->symbol, sizeof(snapshot->symbol), "%s", request->symbol);
  snprintf(snapshot->range, sizeof(snapshot->range), "%s", request->range);
  snapshot->generation = request->generation;
  cJSON *interval = cJSON_GetObjectItemCaseSensitive(root, "interval");
  if (cJSON_IsString(interval))
  {
    snprintf(snapshot->interval, sizeof(snapshot->interval), "%s",
             interval->valuestring);
  }
  cJSON *session_open = cJSON_GetObjectItemCaseSensitive(root, "session_open");
  if (cJSON_IsNumber(session_open))
  {
    snapshot->session_open = (uint32_t)session_open->valuedouble;
  }
  cJSON *session_close = cJSON_GetObjectItemCaseSensitive(root, "session_close");
  if (cJSON_IsNumber(session_close))
  {
    snapshot->session_close = (uint32_t)session_close->valuedouble;
  }

  cJSON *point;
  cJSON_ArrayForEach(point, points)
  {
    cJSON *last = cJSON_GetObjectItemCaseSensitive(point, "last");
    cJSON *timestamp = cJSON_GetObjectItemCaseSensitive(point, "ts");
    if (cJSON_IsNumber(last) && last->valuedouble > 0.0 &&
        snapshot->point_count < STOCK_SPARKLINE_MAX_POINTS)
    {
      size_t index = snapshot->point_count++;
      snapshot->closes[index] = (float)last->valuedouble;
      if (cJSON_IsNumber(timestamp))
      {
        snapshot->timestamps[index] = (uint32_t)timestamp->valuedouble;
      }
    }
  }
  cJSON_Delete(root);

  if (snapshot->point_count < 2U)
  {
    snprintf(error, error_size, "history has no data");
    return -1;
  }
  snapshot->fresh = true;
  snprintf(snapshot->status, sizeof(snapshot->status), "%s history via HTTPS",
           request->range);
  return 0;
}

int stock_api_fetch_logo(const char *symbol, const uint8_t **png,
                         size_t *png_len, char *error, size_t error_size)
{
  char path[96];
  snprintf(path, sizeof(path), "%s/logo/%s?size=48", STOCK_API_BASE_PATH, symbol);

  char *body;
  size_t len = 0;
  if (https_get(path, &body, &len, error, error_size) != 0)
  {
    return -1;
  }
  /* PNG signature check: 89 50 4E 47 0D 0A 1A 0A */
  if (len < 8U || (uint8_t)body[0] != 0x89U || body[1] != 'P' ||
      body[2] != 'N' || body[3] != 'G')
  {
    snprintf(error, error_size, "not a PNG (%u bytes)", (unsigned)len);
    return -1;
  }
  *png = (const uint8_t *)body;
  *png_len = len;
  return 0;
}
