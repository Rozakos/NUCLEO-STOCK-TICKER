#ifndef APP_SETTINGS_H
#define APP_SETTINGS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APP_MAX_SYMBOLS 8U
#define APP_SYMBOL_LENGTH 12U

void settings_init(void);
void settings_storage_load(void);
size_t settings_get_symbols(char symbols[APP_MAX_SYMBOLS][APP_SYMBOL_LENGTH]);
bool settings_set_symbols_csv(const char *csv);
bool settings_add_symbol(const char *symbol);
bool settings_delete_symbol(size_t index);
/* Shares owned per symbol, index-aligned with settings_get_symbols(). */
void settings_get_shares(float shares[APP_MAX_SYMBOLS]);
bool settings_set_shares(size_t index, float quantity);
bool settings_set_shares_csv(const char *csv);
/* Price-alert thresholds, index-aligned with settings_get_symbols().
 * 0 (or negative) disables that side. Both may be set on one symbol. */
void settings_get_alerts(float above[APP_MAX_SYMBOLS],
                         float below[APP_MAX_SYMBOLS]);
bool settings_set_alert(size_t index, float above, float below);
/* CSV of "above|below" pairs, e.g. "150|0,0|90,0|0" - the persisted form. */
bool settings_set_alerts_csv(const char *csv);
uint32_t settings_get_refresh_seconds(void);
bool settings_set_refresh_seconds(uint32_t seconds);
uint32_t settings_generation(void);

#endif /* APP_SETTINGS_H */
