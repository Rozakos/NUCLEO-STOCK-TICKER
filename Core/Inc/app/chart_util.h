#ifndef APP_CHART_UTIL_H
#define APP_CHART_UTIL_H

#include <stddef.h>
#include <stdint.h>

#include "lvgl.h"

/* Calendar fields decoded from a Unix epoch (UTC + configured offset). */
typedef struct
{
  int year;    /* e.g. 2026 */
  int month;   /* 1..12 */
  int day;     /* 1..31 */
  int hour;    /* 0..23 */
  int minute;  /* 0..59 */
} chart_civil_t;

/* Monotone cubic Hermite (Fritsch-Carlson / PCHIP) interpolation with
 * uniform input spacing. Produces `factor` output samples per input segment
 * without the overshoot Catmull-Rom shows on noisy close series. */
void chart_util_monotone_cubic(const float *in, size_t in_n,
                               float *out, size_t out_n, int factor);

/* Rasterize the area between a polyline and `bottom_y` row-by-row with a
 * vertical alpha fade (top_opa at the top of the chart, transparent at the
 * bottom). Row-at-a-time fills mean every pixel of a row shares one alpha,
 * so no vertical banding at segment seams. Coordinates are chart-local;
 * (ox, oy) is the chart's absolute origin. */
void chart_util_draw_polyline_fill(lv_layer_t *layer,
                                   const int32_t *xs, const int32_t *ys,
                                   int n, int32_t ox, int32_t oy,
                                   int32_t bottom_y,
                                   lv_color_t color, lv_opa_t top_opa);

/* Decode a Unix timestamp into calendar fields. No timezone database on
 * this target: the caller passes a fixed offset in minutes east of UTC. */
void chart_util_epoch_to_civil(uint32_t epoch, int utc_offset_minutes,
                               chart_civil_t *out);

#endif /* APP_CHART_UTIL_H */
