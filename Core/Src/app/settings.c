#include "app/settings.h"
#include "app/config.h"
#include "app/format.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cmsis_os.h"
#include "fatfs.h"
#include "FreeRTOS.h"
#include "task.h"
#include "stm32f7xx_hal.h"

/* Serialises settings_save(). FatFs is built reentrant, but HAL_FLASH_* is
 * not, and the ticker.tmp -> ticker.cfg rename is not atomic against a
 * second writer. Mutations now arrive from both the web task and the
 * Bluetooth console, so the save path needs a lock of its own. */
static osMutexId save_lock;

static char current_symbols[APP_MAX_SYMBOLS][APP_SYMBOL_LENGTH];
static float current_shares[APP_MAX_SYMBOLS];  /* owned qty, index-aligned */
static float current_alert_above[APP_MAX_SYMBOLS];  /* 0 = that side is off */
static float current_alert_below[APP_MAX_SYMBOLS];
static size_t current_count;
static uint32_t generation;
static uint32_t refresh_seconds;
static bool storage_ready;
static bool sd_ready;

/* ---- Internal-flash store ------------------------------------------------
 * Boards without a readable SD card persist settings in flash sector 7
 * (256 KB at 0x080C0000), which the linker reserves (FLASH capped at 768K).
 * A sector erase stalls the CPU for ~1 s (single-bank XIP), so saves skip
 * rewriting when the blob is unchanged. */
#define SETTINGS_FLASH_ADDR  0x080C0000U
#define SETTINGS_FLASH_MAGIC 0x53544B32U   /* "STK2": v1 + alert thresholds */
#define SETTINGS_FLASH_MAGIC_V1 0x53544B31U

typedef struct
{
  uint32_t magic;
  uint32_t refresh;
  uint32_t count;
  char symbols[APP_MAX_SYMBOLS][APP_SYMBOL_LENGTH];
  float shares[APP_MAX_SYMBOLS];
  float alert_above[APP_MAX_SYMBOLS];
  float alert_below[APP_MAX_SYMBOLS];
  uint32_t checksum;
} settings_blob_t;

/* The pre-alerts layout. Adding fields moved `checksum`, so a v1 blob fails
 * the v2 check; without this the first boot after upgrading would silently
 * drop the user's watchlist back to compile-time defaults. */
typedef struct
{
  uint32_t magic;
  uint32_t refresh;
  uint32_t count;
  char symbols[APP_MAX_SYMBOLS][APP_SYMBOL_LENGTH];
  float shares[APP_MAX_SYMBOLS];
  uint32_t checksum;
} settings_blob_v1_t;

/* Takes an explicit length so both blob layouts can share it. */
static uint32_t blob_checksum(const void *data, size_t length)
{
  const uint8_t *bytes = (const uint8_t *)data;
  uint32_t sum = 0x5A17U;
  for (size_t i = 0; i < length; ++i)
  {
    sum = sum * 31U + bytes[i];
  }
  return sum;
}

static void flash_save(void)
{
  /* Static: settings mutations all come from the web task, and the web
   * task's stack margin is what overflowed when this lived on it. */
  static settings_blob_t blob;
  memset(&blob, 0, sizeof(blob));
  blob.magic = SETTINGS_FLASH_MAGIC;
  taskENTER_CRITICAL();
  blob.refresh = refresh_seconds;
  blob.count = (uint32_t)current_count;
  memcpy(blob.symbols, current_symbols, sizeof(blob.symbols));
  memcpy(blob.shares, current_shares, sizeof(blob.shares));
  memcpy(blob.alert_above, current_alert_above, sizeof(blob.alert_above));
  memcpy(blob.alert_below, current_alert_below, sizeof(blob.alert_below));
  taskEXIT_CRITICAL();
  blob.checksum = blob_checksum(&blob, offsetof(settings_blob_t, checksum));

  /* The stored copy may sit stale in the D-cache from an earlier read. */
  SCB_InvalidateDCache_by_Addr((uint32_t *)SETTINGS_FLASH_ADDR,
                               (int32_t)sizeof(blob));
  if (memcmp((const void *)SETTINGS_FLASH_ADDR, &blob, sizeof(blob)) == 0)
  {
    return;   /* unchanged: spare the sector and the erase stall */
  }

  HAL_FLASH_Unlock();
  FLASH_EraseInitTypeDef erase = {
    .TypeErase = FLASH_TYPEERASE_SECTORS,
    .Sector = FLASH_SECTOR_7,
    .NbSectors = 1U,
    .VoltageRange = FLASH_VOLTAGE_RANGE_3,
  };
  uint32_t bad_sector;
  HAL_StatusTypeDef status = HAL_FLASHEx_Erase(&erase, &bad_sector);
  const uint32_t *words = (const uint32_t *)&blob;
  for (size_t i = 0; status == HAL_OK && i < sizeof(blob) / 4U; ++i)
  {
    status = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD,
                               SETTINGS_FLASH_ADDR + 4U * i, words[i]);
  }
  HAL_FLASH_Lock();
  SCB_InvalidateDCache_by_Addr((uint32_t *)SETTINGS_FLASH_ADDR,
                               (int32_t)sizeof(blob));
  printf(status == HAL_OK ? "[settings] saved to flash\r\n"
                          : "[settings] flash save failed\r\n");
}

/* Load a v1 (pre-alerts) blob, leaving the thresholds at zero. Keeps a
 * watchlist configured before alerts existed from being wiped on upgrade;
 * the next save rewrites the sector in v2 form. */
static bool flash_load_v1(void)
{
  const settings_blob_v1_t *blob =
      (const settings_blob_v1_t *)SETTINGS_FLASH_ADDR;
  if (blob->count == 0U || blob->count > APP_MAX_SYMBOLS ||
      blob->checksum !=
          blob_checksum(blob, offsetof(settings_blob_v1_t, checksum)))
  {
    return false;
  }

  taskENTER_CRITICAL();
  current_count = blob->count;
  memcpy(current_symbols, blob->symbols, sizeof(current_symbols));
  for (size_t i = 0; i < APP_MAX_SYMBOLS; ++i)
  {
    current_symbols[i][APP_SYMBOL_LENGTH - 1U] = '\0';
    current_shares[i] = blob->shares[i] >= 0.0f ? blob->shares[i] : 0.0f;
    current_alert_above[i] = 0.0f;
    current_alert_below[i] = 0.0f;
  }
  if (blob->refresh >= 15U && blob->refresh <= 3600U)
  {
    refresh_seconds = blob->refresh;
  }
  ++generation;
  taskEXIT_CRITICAL();
  printf("[settings] loaded from flash (pre-alerts format, upgraded)\r\n");
  return true;
}

static bool flash_load(void)
{
  const settings_blob_t *blob = (const settings_blob_t *)SETTINGS_FLASH_ADDR;

  if (blob->magic == SETTINGS_FLASH_MAGIC_V1)
  {
    if (flash_load_v1()) return true;
    printf("[settings] no flash config; using defaults\r\n");
    return false;
  }

  if (blob->magic != SETTINGS_FLASH_MAGIC || blob->count == 0U ||
      blob->count > APP_MAX_SYMBOLS ||
      blob->checksum != blob_checksum(blob,
                                      offsetof(settings_blob_t, checksum)))
  {
    printf("[settings] no flash config; using defaults\r\n");
    return false;
  }

  taskENTER_CRITICAL();
  current_count = blob->count;
  memcpy(current_symbols, blob->symbols, sizeof(current_symbols));
  for (size_t i = 0; i < APP_MAX_SYMBOLS; ++i)
  {
    current_symbols[i][APP_SYMBOL_LENGTH - 1U] = '\0';
    current_shares[i] = blob->shares[i] >= 0.0f ? blob->shares[i] : 0.0f;
    current_alert_above[i] =
        blob->alert_above[i] > 0.0f ? blob->alert_above[i] : 0.0f;
    current_alert_below[i] =
        blob->alert_below[i] > 0.0f ? blob->alert_below[i] : 0.0f;
  }
  if (blob->refresh >= 15U && blob->refresh <= 3600U)
  {
    refresh_seconds = blob->refresh;
  }
  ++generation;
  taskEXIT_CRITICAL();
  printf("[settings] loaded from flash\r\n");
  return true;
}

static void settings_save_locked(void)
{
  flash_save();
  if (!sd_ready) return;

  char symbols[APP_MAX_SYMBOLS][APP_SYMBOL_LENGTH];
  size_t count = settings_get_symbols(symbols);
  uint32_t refresh = settings_get_refresh_seconds();
  char path[24];
  char temp_path[24];
  snprintf(path, sizeof(path), "%sticker.cfg", SDPath);
  snprintf(temp_path, sizeof(temp_path), "%sticker.tmp", SDPath);

  float shares[APP_MAX_SYMBOLS];
  settings_get_shares(shares);

  FIL file;
  if (f_open(&file, temp_path, FA_CREATE_ALWAYS | FA_WRITE) != FR_OK) return;
  f_printf(&file, "refresh=%lu\nsymbols=", (unsigned long)refresh);
  for (size_t i = 0; i < count; ++i)
  {
    f_printf(&file, "%s%s", i == 0U ? "" : ",", symbols[i]);
  }
  /* shares= must follow symbols= so the loader applies it to the right
   * indices (newlib-nano has no %f; format_decimal_2 renders the values). */
  f_printf(&file, "\nshares=");
  for (size_t i = 0; i < count; ++i)
  {
    char quantity[20];
    format_decimal_2(quantity, sizeof(quantity), shares[i], 0);
    f_printf(&file, "%s%s", i == 0U ? "" : ",", quantity);
  }

  /* alerts= is "above|below" per symbol. An older firmware simply ignores
   * the unknown key, and its absence means "no alerts configured". */
  float above[APP_MAX_SYMBOLS];
  float below[APP_MAX_SYMBOLS];
  settings_get_alerts(above, below);
  f_printf(&file, "\nalerts=");
  for (size_t i = 0; i < count; ++i)
  {
    char high[20];
    char low[20];
    format_decimal_2(high, sizeof(high), above[i], 0);
    format_decimal_2(low, sizeof(low), below[i], 0);
    f_printf(&file, "%s%s|%s", i == 0U ? "" : ",", high, low);
  }
  f_printf(&file, "\n");
  FRESULT result = f_sync(&file);
  if (f_close(&file) != FR_OK || result != FR_OK) return;

  f_unlink(path);
  if (f_rename(temp_path, path) == FR_OK)
  {
    printf("[settings] saved to SD\r\n");
  }
}

static void settings_save(void)
{
  if (!storage_ready) return;

  if (save_lock != NULL) osMutexWait(save_lock, osWaitForever);
  settings_save_locked();
  if (save_lock != NULL) osMutexRelease(save_lock);
}

static bool valid_symbol_char(char value)
{
  return isalnum((unsigned char)value) || value == '.' || value == '-' ||
         value == '^';
}

void settings_init(void)
{
  static const char *defaults[] = STOCK_SYMBOLS;

  /* Runs from main() before the scheduler starts, which is exactly when the
   * mutex must exist - by the time two tasks can race, it is already there. */
  if (save_lock == NULL)
  {
    osMutexDef(settingsSaveLock);
    save_lock = osMutexCreate(osMutex(settingsSaveLock));
  }

  taskENTER_CRITICAL();
  current_count = STOCK_SYMBOL_COUNT > APP_MAX_SYMBOLS
      ? APP_MAX_SYMBOLS : STOCK_SYMBOL_COUNT;
  for (size_t i = 0; i < current_count; ++i)
  {
    strncpy(current_symbols[i], defaults[i], APP_SYMBOL_LENGTH - 1U);
    current_symbols[i][APP_SYMBOL_LENGTH - 1U] = '\0';
  }
  generation = 1U;
  refresh_seconds = STOCK_REFRESH_MS / 1000U;
  taskEXIT_CRITICAL();
}

void settings_storage_load(void)
{
  bool loaded = false;

  if (f_mount(&SDFatFS, SDPath, 1) == FR_OK)
  {
    sd_ready = true;
    char path[24];
    snprintf(path, sizeof(path), "%sticker.cfg", SDPath);
    FIL file;
    if (f_open(&file, path, FA_READ) == FR_OK)
    {
      /* 256, not 128: "alerts=" carries two numbers per symbol, so 8
       * symbols can run past 150 characters and a short buffer would
       * silently split the line and corrupt the last entry. */
      char line[256];
      while (f_gets(line, sizeof(line), &file) != NULL)
      {
        char *newline = strpbrk(line, "\r\n");
        if (newline != NULL) *newline = '\0';
        if (strncmp(line, "symbols=", 8) == 0)
        {
          settings_set_symbols_csv(line + 8);
        }
        else if (strncmp(line, "shares=", 7) == 0)
        {
          settings_set_shares_csv(line + 7);
        }
        else if (strncmp(line, "alerts=", 7) == 0)
        {
          settings_set_alerts_csv(line + 7);
        }
        else if (strncmp(line, "refresh=", 8) == 0)
        {
          settings_set_refresh_seconds((uint32_t)strtoul(line + 8, NULL, 10));
        }
      }
      f_close(&file);
      loaded = true;
      printf("[settings] loaded from SD\r\n");
    }
    else
    {
      printf("[settings] no SD config\r\n");
    }
  }
  else
  {
    printf("[settings] SD unavailable\r\n");
  }

  /* No SD (or an empty card): the internal-flash blob is the fallback. */
  if (!loaded)
  {
    flash_load();
  }
  storage_ready = true;
}

size_t settings_get_symbols(char symbols[APP_MAX_SYMBOLS][APP_SYMBOL_LENGTH])
{
  taskENTER_CRITICAL();
  size_t count = current_count;
  memcpy(symbols, current_symbols, sizeof(current_symbols));
  taskEXIT_CRITICAL();
  return count;
}

bool settings_set_symbols_csv(const char *csv)
{
  char parsed[APP_MAX_SYMBOLS][APP_SYMBOL_LENGTH] = { 0 };
  size_t count = 0;

  while (*csv != '\0' && count < APP_MAX_SYMBOLS)
  {
    while (*csv == ' ' || *csv == ',') ++csv;
    size_t length = 0;
    while (*csv != '\0' && *csv != ',')
    {
      if (*csv != ' ')
      {
        if (length >= APP_SYMBOL_LENGTH - 1U || !valid_symbol_char(*csv))
        {
          return false;
        }
        parsed[count][length++] = (char)toupper((unsigned char)*csv);
      }
      ++csv;
    }
    if (length > 0U)
    {
      parsed[count][length] = '\0';
      ++count;
    }
  }

  if (count == 0U)
  {
    return false;
  }

  taskENTER_CRITICAL();
  memcpy(current_symbols, parsed, sizeof(current_symbols));
  /* Full list replace invalidates the index-aligned share counts and alert
   * thresholds; the SD loader restores them from the shares= and alerts=
   * lines that follow symbols=. */
  memset(current_shares, 0, sizeof(current_shares));
  memset(current_alert_above, 0, sizeof(current_alert_above));
  memset(current_alert_below, 0, sizeof(current_alert_below));
  current_count = count;
  ++generation;
  taskEXIT_CRITICAL();
  settings_save();
  return true;
}

bool settings_add_symbol(const char *symbol)
{
  char parsed[APP_SYMBOL_LENGTH] = { 0 };
  size_t length = 0;
  while (*symbol != '\0' && *symbol != '&' && *symbol != '\r' &&
         *symbol != '\n')
  {
    if (*symbol != ' ')
    {
      if (length >= sizeof(parsed) - 1U || !valid_symbol_char(*symbol))
      {
        return false;
      }
      parsed[length++] = (char)toupper((unsigned char)*symbol);
    }
    ++symbol;
  }
  if (length == 0U) return false;

  taskENTER_CRITICAL();
  bool added = false;
  if (current_count < APP_MAX_SYMBOLS)
  {
    bool duplicate = false;
    for (size_t i = 0; i < current_count; ++i)
    {
      if (strcmp(current_symbols[i], parsed) == 0) duplicate = true;
    }
    if (!duplicate)
    {
      current_shares[current_count] = 0.0f;
      current_alert_above[current_count] = 0.0f;
      current_alert_below[current_count] = 0.0f;
      memcpy(current_symbols[current_count++], parsed, sizeof(parsed));
      ++generation;
      added = true;
    }
  }
  taskEXIT_CRITICAL();
  if (added) settings_save();
  return added;
}

bool settings_delete_symbol(size_t index)
{
  taskENTER_CRITICAL();
  bool deleted = false;
  if (current_count > 1U && index < current_count)
  {
    for (size_t i = index; i + 1U < current_count; ++i)
    {
      memcpy(current_symbols[i], current_symbols[i + 1U], APP_SYMBOL_LENGTH);
      current_shares[i] = current_shares[i + 1U];
      current_alert_above[i] = current_alert_above[i + 1U];
      current_alert_below[i] = current_alert_below[i + 1U];
    }
    memset(current_symbols[current_count - 1U], 0, APP_SYMBOL_LENGTH);
    current_shares[current_count - 1U] = 0.0f;
    current_alert_above[current_count - 1U] = 0.0f;
    current_alert_below[current_count - 1U] = 0.0f;
    --current_count;
    ++generation;
    deleted = true;
  }
  taskEXIT_CRITICAL();
  if (deleted) settings_save();
  return deleted;
}

void settings_get_shares(float shares[APP_MAX_SYMBOLS])
{
  taskENTER_CRITICAL();
  memcpy(shares, current_shares, sizeof(current_shares));
  taskEXIT_CRITICAL();
}

bool settings_set_shares(size_t index, float quantity)
{
  /* NaN-safe lower bound: !(x >= 0) is true for NaN. */
  if (!(quantity >= 0.0f) || quantity > 9999999.0f) return false;

  taskENTER_CRITICAL();
  bool ok = index < current_count;
  if (ok)
  {
    current_shares[index] = quantity;
    ++generation;
  }
  taskEXIT_CRITICAL();
  if (ok) settings_save();
  return ok;
}

bool settings_set_shares_csv(const char *csv)
{
  float parsed[APP_MAX_SYMBOLS] = { 0 };
  size_t count = 0;

  while (count < APP_MAX_SYMBOLS && *csv != '\0')
  {
    char *end;
    float value = strtof(csv, &end);
    if (end == csv) break;
    if (!(value >= 0.0f)) value = 0.0f;
    parsed[count++] = value;
    csv = end;
    while (*csv == ',' || *csv == ' ') ++csv;
  }

  taskENTER_CRITICAL();
  memcpy(current_shares, parsed, sizeof(current_shares));
  ++generation;
  taskEXIT_CRITICAL();
  return true;
}

void settings_get_alerts(float above[APP_MAX_SYMBOLS],
                         float below[APP_MAX_SYMBOLS])
{
  taskENTER_CRITICAL();
  memcpy(above, current_alert_above, sizeof(current_alert_above));
  memcpy(below, current_alert_below, sizeof(current_alert_below));
  taskEXIT_CRITICAL();
}

bool settings_set_alert(size_t index, float above, float below)
{
  /* NaN-safe: !(x >= 0) is true for NaN. 0 means "this side is off". */
  if (!(above >= 0.0f) || !(below >= 0.0f)) return false;
  if (above > 9999999.0f || below > 9999999.0f) return false;

  taskENTER_CRITICAL();
  bool ok = index < current_count;
  if (ok)
  {
    current_alert_above[index] = above;
    current_alert_below[index] = below;
    ++generation;
  }
  taskEXIT_CRITICAL();
  if (ok) settings_save();
  return ok;
}

bool settings_set_alerts_csv(const char *csv)
{
  float above[APP_MAX_SYMBOLS] = { 0 };
  float below[APP_MAX_SYMBOLS] = { 0 };
  size_t count = 0;

  /* Entries are "above|below", comma separated. */
  while (count < APP_MAX_SYMBOLS && *csv != '\0')
  {
    char *end;
    float high = strtof(csv, &end);
    if (end == csv) break;
    csv = end;

    float low = 0.0f;
    if (*csv == '|')
    {
      ++csv;
      low = strtof(csv, &end);
      if (end == csv) low = 0.0f;
      else csv = end;
    }

    above[count] = (high >= 0.0f) ? high : 0.0f;
    below[count] = (low >= 0.0f) ? low : 0.0f;
    ++count;

    while (*csv == ',' || *csv == ' ') ++csv;
  }

  taskENTER_CRITICAL();
  memcpy(current_alert_above, above, sizeof(current_alert_above));
  memcpy(current_alert_below, below, sizeof(current_alert_below));
  ++generation;
  taskEXIT_CRITICAL();
  return true;
}

uint32_t settings_get_refresh_seconds(void)
{
  taskENTER_CRITICAL();
  uint32_t seconds = refresh_seconds;
  taskEXIT_CRITICAL();
  return seconds;
}

bool settings_set_refresh_seconds(uint32_t seconds)
{
  if (seconds < 15U || seconds > 3600U) return false;
  taskENTER_CRITICAL();
  refresh_seconds = seconds;
  ++generation;
  taskEXIT_CRITICAL();
  settings_save();
  return true;
}

uint32_t settings_generation(void)
{
  taskENTER_CRITICAL();
  uint32_t result = generation;
  taskEXIT_CRITICAL();
  return result;
}
