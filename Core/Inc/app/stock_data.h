#ifndef APP_STOCK_DATA_H
#define APP_STOCK_DATA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app/settings.h"

#define STOCK_SPARKLINE_MAX_POINTS 64U

typedef enum
{
  STOCK_EXT_NONE = 0,   /* regular session (or no extended data) */
  STOCK_EXT_PRE,        /* pre-market quote active */
  STOCK_EXT_POST,       /* after-hours quote active */
} stock_ext_state_t;

/* The API's market_state field; UNKNOWN when an older API omits it (the
 * parser then falls back to inferring PRE/POST from the price fields). */
typedef enum
{
  STOCK_SESSION_UNKNOWN = 0,
  STOCK_SESSION_PRE,
  STOCK_SESSION_REGULAR,
  STOCK_SESSION_POST,
  STOCK_SESSION_CLOSED,
} stock_session_t;

typedef struct
{
  char symbol[12];
  float last;
  float change_pct;
  stock_ext_state_t ext_state;
  float ext_price;      /* extended-hours print; change is vs regular close */
  float ext_change_pct;
  stock_session_t session;
  float closes[STOCK_SPARKLINE_MAX_POINTS];
  size_t close_count;
  uint32_t updated_ms;
  bool fresh;
  char status[48];
} stock_snapshot_t;

void stock_data_publish(const stock_snapshot_t *snapshot);
void stock_data_reset(void);
bool stock_data_get(stock_snapshot_t *snapshot);
bool stock_data_get_symbol(const char *symbol, stock_snapshot_t *snapshot);
size_t stock_data_get_all(stock_snapshot_t snapshots[APP_MAX_SYMBOLS]);

#endif /* APP_STOCK_DATA_H */
