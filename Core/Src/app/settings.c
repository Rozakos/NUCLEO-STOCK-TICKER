#include "app/settings.h"
#include "app/config.h"
#include "app/format.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fatfs.h"
#include "FreeRTOS.h"
#include "task.h"

static char current_symbols[APP_MAX_SYMBOLS][APP_SYMBOL_LENGTH];
static float current_shares[APP_MAX_SYMBOLS];  /* owned qty, index-aligned */
static size_t current_count;
static uint32_t generation;
static uint32_t refresh_seconds;
static bool storage_ready;

static void settings_save(void)
{
  if (!storage_ready) return;

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
  f_printf(&file, "\n");
  FRESULT result = f_sync(&file);
  if (f_close(&file) != FR_OK || result != FR_OK) return;

  f_unlink(path);
  if (f_rename(temp_path, path) == FR_OK)
  {
    printf("[settings] saved to SD\r\n");
  }
}

static bool valid_symbol_char(char value)
{
  return isalnum((unsigned char)value) || value == '.' || value == '-' ||
         value == '^';
}

void settings_init(void)
{
  static const char *defaults[] = STOCK_SYMBOLS;

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
  if (f_mount(&SDFatFS, SDPath, 1) != FR_OK)
  {
    printf("[settings] SD unavailable; using defaults\r\n");
    return;
  }

  char path[24];
  snprintf(path, sizeof(path), "%sticker.cfg", SDPath);
  FIL file;
  if (f_open(&file, path, FA_READ) == FR_OK)
  {
    char line[128];
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
      else if (strncmp(line, "refresh=", 8) == 0)
      {
        settings_set_refresh_seconds((uint32_t)strtoul(line + 8, NULL, 10));
      }
    }
    f_close(&file);
    printf("[settings] loaded from SD\r\n");
  }
  else
  {
    printf("[settings] no SD config; using defaults\r\n");
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
  /* Full list replace invalidates the index-aligned share counts; the SD
   * loader restores them from the shares= line that follows symbols=. */
  memset(current_shares, 0, sizeof(current_shares));
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
    }
    memset(current_symbols[current_count - 1U], 0, APP_SYMBOL_LENGTH);
    current_shares[current_count - 1U] = 0.0f;
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
