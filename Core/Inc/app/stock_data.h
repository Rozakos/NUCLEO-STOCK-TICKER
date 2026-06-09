#ifndef APP_STOCK_DATA_H
#define APP_STOCK_DATA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app/settings.h"

#define STOCK_SPARKLINE_MAX_POINTS 64U

typedef struct
{
  char symbol[12];
  float last;
  float change_pct;
  float closes[STOCK_SPARKLINE_MAX_POINTS];
  size_t close_count;
  uint32_t updated_ms;
  bool fresh;
  char status[48];
} stock_snapshot_t;

void stock_data_publish(const stock_snapshot_t *snapshot);
void stock_data_reset(void);
bool stock_data_get(stock_snapshot_t *snapshot);
size_t stock_data_get_all(stock_snapshot_t snapshots[APP_MAX_SYMBOLS]);

#endif /* APP_STOCK_DATA_H */
