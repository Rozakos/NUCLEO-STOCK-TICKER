#include "app/settings.h"
#include "app/config.h"

#include <ctype.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

static char current_symbols[APP_MAX_SYMBOLS][APP_SYMBOL_LENGTH];
static size_t current_count;
static uint32_t generation;

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
  taskEXIT_CRITICAL();
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
  current_count = count;
  ++generation;
  taskEXIT_CRITICAL();
  return true;
}

uint32_t settings_generation(void)
{
  taskENTER_CRITICAL();
  uint32_t result = generation;
  taskEXIT_CRITICAL();
  return result;
}
