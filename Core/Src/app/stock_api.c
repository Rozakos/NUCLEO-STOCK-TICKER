#include "app/stock_api.h"
#include "app/config.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

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

#define TLS_HEAP_ADDR       ((unsigned char *)0xC0040000U)
#define TLS_HEAP_SIZE       (128U * 1024U)
#define HTTP_RESPONSE_ADDR  ((char *)0xC0060000U)
#define HTTP_RESPONSE_SIZE  (24U * 1024U)

static bool api_initialized;

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

static int https_get(const char *path, char **body, char *error,
                     size_t error_size)
{
  int result = -1;
  int socket_fd = -1;
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context drbg;
  mbedtls_ssl_context ssl;
  mbedtls_ssl_config config;

  mbedtls_entropy_init(&entropy);
  mbedtls_ctr_drbg_init(&drbg);
  mbedtls_ssl_init(&ssl);
  mbedtls_ssl_config_init(&config);

  int ret = mbedtls_entropy_add_source(&entropy, hardware_entropy, NULL, 32,
                                       MBEDTLS_ENTROPY_SOURCE_STRONG);
  if (ret != 0)
  {
    format_tls_error(error, error_size, "entropy", ret);
    goto cleanup;
  }

  static const unsigned char personalization[] = "nucleo-stock-ticker";
  ret = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                              personalization, sizeof(personalization) - 1U);
  if (ret != 0)
  {
    format_tls_error(error, error_size, "DRBG", ret);
    goto cleanup;
  }

  ret = mbedtls_ssl_config_defaults(&config, MBEDTLS_SSL_IS_CLIENT,
                                    MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT);
  if (ret != 0)
  {
    format_tls_error(error, error_size, "TLS config", ret);
    goto cleanup;
  }
  mbedtls_ssl_conf_authmode(&config, MBEDTLS_SSL_VERIFY_NONE);
  mbedtls_ssl_conf_rng(&config, mbedtls_ctr_drbg_random, &drbg);

  ret = mbedtls_ssl_setup(&ssl, &config);
  if (ret != 0)
  {
    format_tls_error(error, error_size, "TLS setup", ret);
    goto cleanup;
  }
  ret = mbedtls_ssl_set_hostname(&ssl, STOCK_API_HOST);
  if (ret != 0)
  {
    format_tls_error(error, error_size, "TLS hostname", ret);
    goto cleanup;
  }

  socket_fd = connect_socket(STOCK_API_HOST, STOCK_API_PORT, error, error_size);
  if (socket_fd < 0)
  {
    goto cleanup;
  }
  mbedtls_ssl_set_bio(&ssl, &socket_fd, socket_send, socket_recv, NULL);

  while ((ret = mbedtls_ssl_handshake(&ssl)) != 0)
  {
    if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE)
    {
      format_tls_error(error, error_size, "TLS handshake", ret);
      goto cleanup;
    }
  }

  char request[768];
  int request_len = snprintf(request, sizeof(request),
      "GET %s HTTP/1.0\r\n"
      "Host: %s\r\n"
      "User-Agent: NUCLEO-Stock-Ticker/1.0 (STM32F746)\r\n"
      "Accept: application/json\r\n"
      "Accept-Encoding: identity\r\n"
      "Authorization: Bearer %s\r\n"
      "Connection: close\r\n\r\n",
      path, STOCK_API_HOST, STOCK_API_TOKEN);
  if (request_len <= 0 || (size_t)request_len >= sizeof(request))
  {
    snprintf(error, error_size, "request too large");
    goto cleanup;
  }

  size_t sent = 0;
  while (sent < (size_t)request_len)
  {
    ret = mbedtls_ssl_write(&ssl, (unsigned char *)request + sent,
                            (size_t)request_len - sent);
    if (ret > 0)
    {
      sent += (size_t)ret;
    }
    else if (ret != MBEDTLS_ERR_SSL_WANT_READ &&
             ret != MBEDTLS_ERR_SSL_WANT_WRITE)
    {
      format_tls_error(error, error_size, "HTTPS write", ret);
      goto cleanup;
    }
  }

  size_t used = 0;
  while (used < HTTP_RESPONSE_SIZE - 1U)
  {
    ret = mbedtls_ssl_read(&ssl, (unsigned char *)HTTP_RESPONSE_ADDR + used,
                           HTTP_RESPONSE_SIZE - 1U - used);
    if (ret > 0)
    {
      used += (size_t)ret;
      continue;
    }
    if (ret == 0 || ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
    {
      break;
    }
    if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
    {
      continue;
    }
    format_tls_error(error, error_size, "HTTPS read", ret);
    goto cleanup;
  }
  HTTP_RESPONSE_ADDR[used] = '\0';

  if (strncmp(HTTP_RESPONSE_ADDR, "HTTP/1.0 200 ", 13) != 0 &&
      strncmp(HTTP_RESPONSE_ADDR, "HTTP/1.1 200 ", 13) != 0)
  {
    char *line_end = strstr(HTTP_RESPONSE_ADDR, "\r\n");
    if (line_end != NULL)
    {
      *line_end = '\0';
    }
    snprintf(error, error_size, "%.40s", HTTP_RESPONSE_ADDR);
    goto cleanup;
  }

  *body = strstr(HTTP_RESPONSE_ADDR, "\r\n\r\n");
  if (*body == NULL)
  {
    snprintf(error, error_size, "bad HTTP response");
    goto cleanup;
  }
  *body += 4;
  result = 0;

cleanup:
  if (socket_fd >= 0)
  {
    mbedtls_ssl_close_notify(&ssl);
    closesocket(socket_fd);
  }
  mbedtls_ssl_free(&ssl);
  mbedtls_ssl_config_free(&config);
  mbedtls_ctr_drbg_free(&drbg);
  mbedtls_entropy_free(&entropy);
  return result;
}

void stock_api_init(void)
{
  if (api_initialized)
  {
    return;
  }

  mbedtls_memory_buffer_alloc_init(TLS_HEAP_ADDR, TLS_HEAP_SIZE);
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
  if (https_get(path, &body, error, error_size) != 0)
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

int stock_api_fetch_history(const history_request_t *request,
                            history_snapshot_t *snapshot,
                            char *error, size_t error_size)
{
  char path[128];
  snprintf(path, sizeof(path), "%s/history/%s?range=%s&limit=%u",
           STOCK_API_BASE_PATH, request->symbol, request->range,
           STOCK_SPARKLINE_MAX_POINTS);

  char *body;
  if (https_get(path, &body, error, error_size) != 0)
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
