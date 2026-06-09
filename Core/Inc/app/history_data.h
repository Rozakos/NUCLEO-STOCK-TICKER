#ifndef APP_HISTORY_DATA_H
#define APP_HISTORY_DATA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app/settings.h"
#include "app/stock_data.h"

#define HISTORY_RANGE_LENGTH 5U
#define HISTORY_INTERVAL_LENGTH 12U

typedef struct
{
  char symbol[APP_SYMBOL_LENGTH];
  char range[HISTORY_RANGE_LENGTH];
  uint32_t generation;
} history_request_t;

typedef struct
{
  char symbol[APP_SYMBOL_LENGTH];
  char range[HISTORY_RANGE_LENGTH];
  char interval[HISTORY_INTERVAL_LENGTH];
  float closes[STOCK_SPARKLINE_MAX_POINTS];
  uint32_t timestamps[STOCK_SPARKLINE_MAX_POINTS];
  size_t point_count;
  uint32_t generation;
  bool fresh;
  bool error;
  char status[48];
} history_snapshot_t;

uint32_t history_data_request(const char *symbol, const char *range);
bool history_data_request_pending(void);
bool history_data_take_request(history_request_t *request);
bool history_data_request_current(uint32_t generation);
void history_data_publish(const history_snapshot_t *snapshot);
void history_data_publish_error(const history_request_t *request,
                                const char *status);
bool history_data_get(history_snapshot_t *snapshot);

#endif /* APP_HISTORY_DATA_H */
