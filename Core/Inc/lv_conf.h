#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_DEPTH 16

/* LVGL heap + image cache live in external SDRAM so on-device PNG decode
 * (lodepng) has plenty of transient room and we don't pressure the 320 KB
 * internal RAM. SDRAM map: FB0 0xC0000000, TLS/HTTP/web 0xC0040000..0xC006D000,
 * FB1 0xC0080000, LVGL heap 0xC0100000..0xC0300000 (2 MB), logo pool 0xC0300000
 * (see app/logo_cache.c). */
#define LV_MEM_SIZE (2048U * 1024U)
#define LV_MEM_ADR  0xC0100000U
#define LV_USE_OS LV_OS_NONE

/* Cache decoded images so logos aren't re-decoded every frame. */
#define LV_CACHE_DEF_SIZE (512U * 1024U)

/* PNG decoder (in-memory) for company logos fetched from GET /logo/{symbol}. */
#define LV_USE_LODEPNG 1

#define LV_USE_LOG 0
#define LV_USE_ASSERT_NULL 1
#define LV_USE_ASSERT_MALLOC 0
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
