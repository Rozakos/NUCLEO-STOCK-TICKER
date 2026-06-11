#include "app/logo_cache.h"

#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

/* SDRAM pool for raw PNG bytes, just past the LVGL heap+cache (which ends at
 * 0xC0300000) and clear of the framebuffers and TLS/HTTP/web buffers. 16 KiB per
 * slot (x8 = 128 KiB, ends 0xC0320000) easily holds a 48x48 PNG. */
#define LOGO_POOL_ADDR  0xC0300000U
#define LOGO_SLOT_SIZE  (16U * 1024U)

enum { SLOT_EMPTY = 0, SLOT_READY, SLOT_FAILED };

/* A transient TLS/HTTP failure shouldn't blank the logo until reboot:
 * retry a failed fetch on later refresh cycles, up to this many attempts. */
#define LOGO_MAX_ATTEMPTS 3U

typedef struct
{
  char symbol[APP_SYMBOL_LENGTH];
  uint8_t *png;            /* -> SDRAM slot (assigned on first use) */
  volatile uint8_t state;
  uint8_t attempts;
  lv_image_dsc_t dsc;
} logo_slot_t;

static logo_slot_t slots[APP_MAX_SYMBOLS];  /* .bss -> all SLOT_EMPTY at boot */

/* Find an existing slot for `symbol` (caller holds the critical section). */
static logo_slot_t *find_slot(const char *symbol)
{
  for (size_t i = 0; i < APP_MAX_SYMBOLS; ++i)
  {
    if (slots[i].symbol[0] != '\0' && strcmp(slots[i].symbol, symbol) == 0)
    {
      return &slots[i];
    }
  }
  return NULL;
}

/* Find or claim a slot for `symbol`, binding its SDRAM region (caller holds
 * the critical section). */
static logo_slot_t *find_or_assign(const char *symbol)
{
  logo_slot_t *slot = find_slot(symbol);
  if (slot != NULL)
  {
    return slot;
  }
  for (size_t i = 0; i < APP_MAX_SYMBOLS; ++i)
  {
    if (slots[i].symbol[0] == '\0')
    {
      strncpy(slots[i].symbol, symbol, APP_SYMBOL_LENGTH - 1U);
      slots[i].symbol[APP_SYMBOL_LENGTH - 1U] = '\0';
      slots[i].png = (uint8_t *)(LOGO_POOL_ADDR + i * LOGO_SLOT_SIZE);
      slots[i].state = SLOT_EMPTY;
      return &slots[i];
    }
  }
  return NULL;
}

const lv_image_dsc_t *logo_cache_lookup(const char *symbol)
{
  const lv_image_dsc_t *dsc = NULL;
  taskENTER_CRITICAL();
  logo_slot_t *slot = find_slot(symbol);
  if (slot != NULL && slot->state == SLOT_READY)
  {
    dsc = &slot->dsc;
  }
  taskEXIT_CRITICAL();
  return dsc;
}

bool logo_cache_should_fetch(const char *symbol)
{
  taskENTER_CRITICAL();
  logo_slot_t *slot = find_or_assign(symbol);
  bool fetch = (slot != NULL &&
                (slot->state == SLOT_EMPTY ||
                 (slot->state == SLOT_FAILED &&
                  slot->attempts < LOGO_MAX_ATTEMPTS)));
  taskEXIT_CRITICAL();
  return fetch;
}

void logo_cache_store(const char *symbol, const uint8_t *png, size_t len)
{
  if (len == 0U || len > LOGO_SLOT_SIZE)
  {
    logo_cache_mark_failed(symbol);
    return;
  }

  taskENTER_CRITICAL();
  logo_slot_t *slot = find_or_assign(symbol);
  uint8_t *dst = (slot != NULL) ? slot->png : NULL;
  taskEXIT_CRITICAL();
  if (dst == NULL)
  {
    return;
  }

  /* Large copy done outside the critical section; state flips to READY only
   * after the bytes and descriptor are fully written, so ui_task never sees a
   * half-written logo. */
  memcpy(dst, png, len);
  memset(&slot->dsc, 0, sizeof(slot->dsc));
  slot->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
  slot->dsc.header.cf = LV_COLOR_FORMAT_RAW;  /* lodepng decodes PNG bytes */
  slot->dsc.data = slot->png;
  slot->dsc.data_size = len;

  taskENTER_CRITICAL();
  slot->state = SLOT_READY;
  taskEXIT_CRITICAL();
}

void logo_cache_mark_failed(const char *symbol)
{
  taskENTER_CRITICAL();
  logo_slot_t *slot = find_or_assign(symbol);
  if (slot != NULL)
  {
    slot->state = SLOT_FAILED;
    if (slot->attempts < LOGO_MAX_ATTEMPTS)
    {
      slot->attempts++;
    }
  }
  taskEXIT_CRITICAL();
}
