#include "app/format.h"

#include <stdint.h>
#include <stdio.h>

void format_decimal_2(char *buffer, size_t buffer_size, float value,
                      int include_sign)
{
  char sign = '\0';
  if (value < 0.0f)
  {
    sign = '-';
    value = -value;
  }
  else if (include_sign)
  {
    sign = '+';
  }

  uint32_t scaled = (uint32_t)(value * 100.0f + 0.5f);
  uint32_t whole = scaled / 100U;
  uint32_t fraction = scaled % 100U;
  if (sign != '\0')
  {
    snprintf(buffer, buffer_size, "%c%lu.%02lu", sign,
             (unsigned long)whole, (unsigned long)fraction);
  }
  else
  {
    snprintf(buffer, buffer_size, "%lu.%02lu",
             (unsigned long)whole, (unsigned long)fraction);
  }
}
