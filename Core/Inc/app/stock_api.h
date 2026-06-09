#ifndef APP_STOCK_API_H
#define APP_STOCK_API_H

#include <stddef.h>

#include "app/history_data.h"
#include "app/stock_data.h"

void stock_api_init(void);
int stock_api_fetch_quote(const char *symbol, stock_snapshot_t *snapshot,
                          char *error, size_t error_size);
int stock_api_fetch_history(const history_request_t *request,
                            history_snapshot_t *snapshot,
                            char *error, size_t error_size);

#endif /* APP_STOCK_API_H */
