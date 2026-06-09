#ifndef APP_SETTINGS_H
#define APP_SETTINGS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APP_MAX_SYMBOLS 8U
#define APP_SYMBOL_LENGTH 12U

void settings_init(void);
size_t settings_get_symbols(char symbols[APP_MAX_SYMBOLS][APP_SYMBOL_LENGTH]);
bool settings_set_symbols_csv(const char *csv);
uint32_t settings_generation(void);

#endif /* APP_SETTINGS_H */
