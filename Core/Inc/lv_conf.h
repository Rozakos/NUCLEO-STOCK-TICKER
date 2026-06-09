#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_DEPTH 16

/* Keep LVGL's allocator in internal RAM. The built-in TLSF allocator hangs on
 * its first allocation when its pool is placed in external SDRAM. */
#define LV_MEM_SIZE (64U * 1024U)
#define LV_USE_OS LV_OS_NONE

/* PNG decoding needs a much larger heap and remains disabled until the
 * external-SDRAM allocator path is made reliable. */
#define LV_USE_LODEPNG 0

#define LV_USE_LOG 0
#define LV_USE_ASSERT_NULL 1
#define LV_USE_ASSERT_MALLOC 1
#define LV_USE_ASSERT_STYLE 0
#define LV_USE_ASSERT_MEM_INTEGRITY 0
#define LV_USE_ASSERT_OBJ 0

#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_18 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_28 1

#define LV_USE_THEME_DEFAULT 1
#define LV_USE_LABEL 1
#define LV_USE_BUTTON 1
#define LV_USE_CHART 1
#define LV_USE_LINE 1

#endif /* LV_CONF_H */
