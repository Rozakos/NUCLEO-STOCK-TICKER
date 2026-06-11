#include "app/chart_util.h"

#include <math.h>

#include "app/stock_data.h"

/* Slope buffers sized for the largest input series. Static because the
 * render path runs on every range switch and the heap is better spent on
 * TLS; only the UI task calls these. */
#define CHART_UTIL_MAX_N (STOCK_SPARKLINE_MAX_POINTS)
static float secant[CHART_UTIL_MAX_N];   /* d_i = y[i+1] - y[i], h = 1 */
static float tangent[CHART_UTIL_MAX_N];  /* m_i, slope at each input knot */

void chart_util_monotone_cubic(const float *in, size_t in_n,
                               float *out, size_t out_n, int factor)
{
  if (in == NULL || out == NULL || out_n == 0U) return;
  if (in_n == 0U) return;
  if (in_n == 1U)
  {
    for (size_t k = 0; k < out_n; ++k) out[k] = in[0];
    return;
  }
  if (in_n > CHART_UTIL_MAX_N) in_n = CHART_UTIL_MAX_N;
  const size_t segs = in_n - 1U;

  for (size_t i = 0; i < segs; ++i)
  {
    secant[i] = in[i + 1U] - in[i];
  }

  /* Fritsch-Carlson initial tangents: zero at local extrema (kills
   * overshoot on noisy data), mean of adjacent secants elsewhere,
   * one-sided difference at the endpoints. */
  tangent[0] = secant[0];
  tangent[segs] = secant[segs - 1U];
  for (size_t i = 1; i < segs; ++i)
  {
    float d_prev = secant[i - 1U];
    float d_next = secant[i];
    tangent[i] = (d_prev * d_next <= 0.0f) ? 0.0f : 0.5f * (d_prev + d_next);
  }

  /* Enforce monotonicity: scale (alpha, beta) into the circle
   * alpha^2 + beta^2 <= 9, the Fritsch-Carlson sufficient region. */
  for (size_t i = 0; i < segs; ++i)
  {
    float d = secant[i];
    if (d == 0.0f)
    {
      tangent[i] = 0.0f;
      tangent[i + 1U] = 0.0f;
      continue;
    }
    float alpha = tangent[i] / d;
    float beta = tangent[i + 1U] / d;
    float s = alpha * alpha + beta * beta;
    if (s > 9.0f)
    {
      float t = 3.0f / sqrtf(s);
      tangent[i] = t * alpha * d;
      tangent[i + 1U] = t * beta * d;
    }
  }

  /* Hermite cubic per segment; h = 1 so tangents need no rescale. */
  size_t write = 0;
  for (size_t seg = 0; seg < segs && write < out_n; ++seg)
  {
    float y0 = in[seg];
    float y1 = in[seg + 1U];
    float m0 = tangent[seg];
    float m1 = tangent[seg + 1U];
    for (int k = 0; k < factor && write < out_n; ++k)
    {
      float t = (float)k / (float)factor;
      float t2 = t * t;
      float t3 = t2 * t;
      out[write++] = (2.0f * t3 - 3.0f * t2 + 1.0f) * y0 +
                     (t3 - 2.0f * t2 + t) * m0 +
                     (-2.0f * t3 + 3.0f * t2) * y1 +
                     (t3 - t2) * m1;
    }
  }
  if (write < out_n) out[write] = in[in_n - 1U];
}

void chart_util_draw_polyline_fill(lv_layer_t *layer,
                                   const int32_t *xs, const int32_t *ys,
                                   int n, int32_t ox, int32_t oy,
                                   int32_t bottom_y,
                                   lv_color_t color, lv_opa_t top_opa)
{
  if (layer == NULL || xs == NULL || ys == NULL) return;
  if (n < 2 || bottom_y <= 0) return;

  lv_draw_rect_dsc_t row_dsc;
  lv_draw_rect_dsc_init(&row_dsc);
  row_dsc.bg_color = color;
  row_dsc.border_width = 0;
  row_dsc.bg_grad.dir = LV_GRAD_DIR_NONE;
  row_dsc.bg_grad.stops_count = 0;

  /* Worst case one crossing per segment of the smoothed series. */
#define MAX_CROSSINGS 384
  static int32_t crossings[MAX_CROSSINGS];

  const int32_t x_end = xs[n - 1];

  for (int32_t y = 0; y < bottom_y; ++y)
  {
    int alpha = ((int)top_opa * (bottom_y - y)) / bottom_y;
    if (alpha <= 0) continue;
    row_dsc.bg_opa = (lv_opa_t)alpha;

    /* Half-open bracketing counts each crossing exactly once. */
    int n_cross = 0;
    for (int i = 0; i + 1 < n; ++i)
    {
      int32_t y0 = ys[i];
      int32_t y1 = ys[i + 1];
      if ((y0 < y && y1 >= y) || (y0 >= y && y1 < y))
      {
        int32_t dy = y1 - y0;
        if (dy == 0) continue;
        if (n_cross < MAX_CROSSINGS)
        {
          crossings[n_cross++] = xs[i] + ((xs[i + 1] - xs[i]) * (y - y0)) / dy;
        }
      }
    }

    /* Insertion sort; n_cross is typically 0..2. */
    for (int i = 1; i < n_cross; ++i)
    {
      int32_t v = crossings[i];
      int j = i;
      while (j > 0 && crossings[j - 1] > v)
      {
        crossings[j] = crossings[j - 1];
        --j;
      }
      crossings[j] = v;
    }

    bool in_fill = (ys[0] < y);
    int32_t x_curr = xs[0];
    for (int k = 0; k < n_cross; ++k)
    {
      int32_t x_next = crossings[k];
      if (in_fill && x_next > x_curr)
      {
        lv_area_t r = { ox + x_curr, oy + y, ox + x_next - 1, oy + y };
        lv_draw_rect(layer, &row_dsc, &r);
      }
      x_curr = x_next;
      in_fill = !in_fill;
    }
    if (in_fill && x_end > x_curr)
    {
      lv_area_t r = { ox + x_curr, oy + y, ox + x_end, oy + y };
      lv_draw_rect(layer, &row_dsc, &r);
    }
  }
}

void chart_util_epoch_to_civil(uint32_t epoch, int utc_offset_minutes,
                               chart_civil_t *out)
{
  int64_t shifted = (int64_t)epoch + (int64_t)utc_offset_minutes * 60;
  if (shifted < 0) shifted = 0;
  uint32_t days = (uint32_t)(shifted / 86400);
  uint32_t secs = (uint32_t)(shifted % 86400);
  out->hour = (int)(secs / 3600U);
  out->minute = (int)((secs % 3600U) / 60U);

  /* Howard Hinnant's civil_from_days (proleptic Gregorian). */
  int64_t z = (int64_t)days + 719468;
  int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  uint32_t doe = (uint32_t)(z - era * 146097);
  uint32_t yoe = (doe - doe / 1460U + doe / 36524U - doe / 146096U) / 365U;
  int64_t y = (int64_t)yoe + era * 400;
  uint32_t doy = doe - (365U * yoe + yoe / 4U - yoe / 100U);
  uint32_t mp = (5U * doy + 2U) / 153U;
  uint32_t d = doy - (153U * mp + 2U) / 5U + 1U;
  uint32_t m = mp < 10U ? mp + 3U : mp - 9U;
  out->year = (int)(y + (m <= 2U));
  out->month = (int)m;
  out->day = (int)d;
}
