#ifndef APP_FORMAT_H
#define APP_FORMAT_H

#include <stddef.h>

void format_decimal_2(char *buffer, size_t buffer_size, float value,
                      int include_sign);

#endif /* APP_FORMAT_H */
