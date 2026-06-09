/**
 * logo_cache.h - per-symbol company-logo cache.
 *
 * net_task fetches the 48x48 PNG for each symbol (GET /logo/{symbol}) and stores
 * the bytes here in an SDRAM pool; ui_task looks them up and hands the PNG to
 * LVGL (lodepng decodes from memory). Thread-safe via FreeRTOS critical sections.
 */
#ifndef APP_LOGO_CACHE_H
#define APP_LOGO_CACHE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app/settings.h"   /* APP_MAX_SYMBOLS, APP_SYMBOL_LENGTH */
#include "lvgl.h"

/* ui_task: ready LVGL image descriptor for `symbol`, or NULL if not cached. */
const lv_image_dsc_t *logo_cache_lookup(const char *symbol);

/* net_task: true if this symbol's logo has not been fetched/attempted yet. */
bool logo_cache_should_fetch(const char *symbol);

/* net_task: copy fetched PNG bytes into the symbol's SDRAM slot (marks ready). */
void logo_cache_store(const char *symbol, const uint8_t *png, size_t len);

/* net_task: record a failed fetch so we don't retry forever. */
void logo_cache_mark_failed(const char *symbol);

#endif /* APP_LOGO_CACHE_H */
