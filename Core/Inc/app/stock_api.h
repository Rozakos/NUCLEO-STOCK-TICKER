#ifndef APP_STOCK_API_H
#define APP_STOCK_API_H

#include <stddef.h>
#include <stdint.h>

#include "app/history_data.h"
#include "app/stock_data.h"

void stock_api_init(void);
int stock_api_fetch_quote(const char *symbol, stock_snapshot_t *snapshot,
                          char *error, size_t error_size);
/* One GET /stocks?symbols=A,B,C for the whole watchlist. Fills up to
 * `count` snapshots (symbols the API couldn't quote are simply absent)
 * and reports how many were returned via `*fetched_count`. */
int stock_api_fetch_quotes(char symbols[APP_MAX_SYMBOLS][APP_SYMBOL_LENGTH],
                           size_t count, stock_snapshot_t *snapshots,
                           size_t *fetched_count, char *error,
                           size_t error_size);
int stock_api_fetch_history(const history_request_t *request,
                            history_snapshot_t *snapshot,
                            char *error, size_t error_size);
/* Fetch the 48x48 PNG for `symbol` (GET /logo/{symbol}?size=48). On success
 * `*png`/`*png_len` point at the PNG bytes in the shared HTTP response buffer,
 * which is overwritten by the next fetch — copy them out immediately. */
int stock_api_fetch_logo(const char *symbol, const uint8_t **png,
                         size_t *png_len, char *error, size_t error_size);

#endif /* APP_STOCK_API_H */
