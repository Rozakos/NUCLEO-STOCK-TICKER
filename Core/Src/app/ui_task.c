#include "app/ui_task.h"
#include "app/chart_util.h"
#include "app/config.h"
#include "app/format.h"
#include "app/history_data.h"
#include "app/logo_cache.h"
#include "app/logos.h"
#include "app/settings.h"
#include "app/stock_data.h"
#include "app/touch_ft5336.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cmsis_os.h"
#include "lvgl.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "main.h"

#define FRAMEBUFFER_SIZE (LCD_WIDTH * LCD_HEIGHT * 2U)
/* Two full framebuffers in SDRAM for tear-free double buffering. FB0 is the
 * LTDC's initial buffer (0xC0000000). FB1 sits 512 KiB in, clear of FB0 and the
 * TLS/HTTP/web buffers (which live at 0xC0040000..0xC006D000 in stock_api.c /
 * web_task.c). LVGL renders into the back buffer; flush swaps the LTDC to it. */
#define FRAMEBUFFER_0 ((void *)(LCD_FB_BASE_ADDR))
#define FRAMEBUFFER_1 ((void *)(LCD_FB_BASE_ADDR + 0x00080000U))
#define STATUS_HEIGHT 28
#define ROW_HEIGHT 54
#define ROW_GAP 5
#define SPARK_WIDTH 150
#define SPARK_HEIGHT 34

/* Detail screen layout (CYD detail_screen port, scaled to 480x272).
 * Screen pad 8 -> 464x256 content: header row, range-button row, then a
 * card holding the chart plus its Y-label gutter and X-label strip. */
#define DETAIL_PAD 8
#define DETAIL_CONTENT_W (LCD_WIDTH - 2 * DETAIL_PAD)
#define DETAIL_HEADER_H 40
#define DETAIL_BTN_ROW_H 26
#define DETAIL_CARD_Y (DETAIL_HEADER_H + 2 + DETAIL_BTN_ROW_H + 2)
#define DETAIL_CARD_H (LCD_HEIGHT - 2 * DETAIL_PAD - DETAIL_CARD_Y)
#define DETAIL_CARD_PAD 8
#define DETAIL_CHART_W (DETAIL_CONTENT_W - 2 * DETAIL_CARD_PAD)
#define DETAIL_CHART_H (DETAIL_CARD_H - 2 * DETAIL_CARD_PAD)
#define DETAIL_MAX_Y_TICKS 8
#define DETAIL_X_TICKS 4
/* Monotone-cubic smoothing density: output samples per input segment. */
#define DETAIL_CR_FACTOR 5
#define DETAIL_CR_MAX_OUT ((STOCK_SPARKLINE_MAX_POINTS - 1U) * DETAIL_CR_FACTOR + 1U)
#define DETAIL_MARKER_SIZE 8
#define DETAIL_NUM_RANGES 7

extern struct netif gnetif;
extern LTDC_HandleTypeDef hltdc;

typedef struct
{
  char symbol[APP_SYMBOL_LENGTH];
  lv_obj_t *row;
  lv_obj_t *badge;
  lv_obj_t *symbol_label;
  lv_obj_t *spark;
  lv_obj_t *price;
  lv_obj_t *change;
  bool logo_applied;   /* true once a real logo (bundled or fetched) is shown */
  lv_point_precise_t points[STOCK_SPARKLINE_MAX_POINTS];
} market_row_t;

static market_row_t rows[APP_MAX_SYMBOLS];
static size_t row_count;
static uint32_t settings_seen;
static lv_obj_t *list;
static lv_obj_t *link_label;
static lv_obj_t *updated_label;
static lv_obj_t *market_screen;
static lv_obj_t *detail_screen;
static lv_obj_t *detail_card;
static lv_obj_t *detail_price;
static lv_obj_t *detail_change;
static lv_obj_t *detail_chart;
static lv_chart_series_t *detail_series;
static lv_obj_t *detail_spinner;
static lv_obj_t *detail_error_label;
static lv_obj_t *detail_marker;
static lv_obj_t *detail_y_labels[DETAIL_MAX_Y_TICKS];
static lv_obj_t *detail_x_labels[DETAIL_X_TICKS];
static lv_obj_t *detail_range_buttons[DETAIL_NUM_RANGES];
static char detail_symbol[APP_SYMBOL_LENGTH];
static uint32_t detail_history_generation;  /* generation of the in-flight request */
static uint32_t detail_done_generation;     /* last generation rendered or errored */
static bool detail_window_rendered;         /* a history window is on the chart */
/* detail_range_displayed = range whose data is on the chart;
 * detail_range_pending = range the user last tapped (highlight follows the
 * tap instantly, data catches up when the fetch lands). */
static int detail_range_displayed;
static int detail_range_pending;
static lv_color_t detail_line_color;

/* Geometry handed to the gradient area-fill draw callback (chart-local). */
static int detail_fill_n;
static int32_t detail_fill_x[DETAIL_CR_MAX_OUT];
static int32_t detail_fill_y[DETAIL_CR_MAX_OUT];
static int32_t detail_fill_bottom;

/* Button label i shows range_labels[i]; the API gets range_api[i]. */
static const char *range_labels[DETAIL_NUM_RANGES] = {
  "1D", "1W", "1M", "6M", "1Y", "5Y", "Max"
};
static const char *range_api[DETAIL_NUM_RANGES] = {
  "1d", "1w", "1mo", "6mo", "1y", "5y", "max"
};
static const char *month_names[12] = {
  "Jan", "Feb", "Mar", "Apr", "May", "Jun",
  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};
/* Y-axis tick steps; pick the smallest giving <= 5 intervals. */
static const float y_steps[] = {
  0.1f, 0.2f, 0.5f, 1, 2, 5, 10, 20, 25, 50, 100, 200, 500, 1000, 2000, 5000
};

static void update_detail(void);
static void create_detail_screen(const char *symbol);

static void row_clicked(lv_event_t *event)
{
  const market_row_t *market = lv_event_get_user_data(event);
  printf("[ui] row clicked: %s\r\n", market->symbol);
  create_detail_screen(market->symbol);
}

static void back_clicked(lv_event_t *event)
{
  (void)event;
  lv_screen_load(market_screen);
  lv_obj_delete_async(detail_screen);
  detail_screen = NULL;
  detail_card = NULL;
  detail_chart = NULL;
  detail_series = NULL;
  detail_spinner = NULL;
  detail_error_label = NULL;
  detail_marker = NULL;
  memset(detail_range_buttons, 0, sizeof(detail_range_buttons));
}

/* Highlight the pending (just-tapped) range button with the chart accent;
 * mute the rest. Tracking the PENDING index means the highlight acknowledges
 * the tap instantly, before the fetch completes. */
static void apply_range_styles(void)
{
  for (int i = 0; i < DETAIL_NUM_RANGES; ++i)
  {
    lv_obj_t *button = detail_range_buttons[i];
    if (button == NULL) continue;
    if (i == detail_range_pending)
    {
      lv_obj_set_style_bg_color(button, detail_line_color, 0);
      lv_obj_set_style_text_color(button, lv_color_hex(0x0B0F17), 0);
    }
    else
    {
      lv_obj_set_style_bg_color(button, lv_color_hex(0x263244), 0);
      lv_obj_set_style_text_color(button, lv_color_hex(0xE7EEF7), 0);
    }
  }
}

/* Blank the chart and pop the spinner so a range switch reads as "loading"
 * immediately, then queue the HTTPS fetch on the network task. */
static void start_history_fetch(void)
{
  detail_window_rendered = false;
  lv_chart_set_all_values(detail_chart, detail_series, LV_CHART_POINT_NONE);
  detail_fill_n = 0;
  lv_obj_add_flag(detail_marker, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(detail_error_label, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(detail_spinner, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(detail_spinner);
  lv_obj_invalidate(detail_chart);
  detail_history_generation =
      history_data_request(detail_symbol, range_api[detail_range_pending]);
  printf("[ui] history range requested: %s\r\n",
         range_api[detail_range_pending]);
}

static void range_clicked(lv_event_t *event)
{
  int index = (int)(uintptr_t)lv_event_get_user_data(event);
  if (index < 0 || index >= DETAIL_NUM_RANGES) return;
  /* Ignore taps on the already-displayed range unless the last fetch
   * failed (the error label is up) — then the tap retries it. */
  if (index == detail_range_displayed &&
      lv_obj_has_flag(detail_error_label, LV_OBJ_FLAG_HIDDEN))
  {
    return;
  }
  detail_range_pending = index;
  apply_range_styles();
  start_history_fetch();
}

static void display_flush(lv_display_t *display, const lv_area_t *area,
                          uint8_t *pixels)
{
  (void)area;
  /* Double-buffered page flip: LVGL just finished rendering into `pixels`
   * (the back buffer). Point the LTDC at it during vertical blanking so the
   * swap is tear-free, then LVGL renders the next frame into the other buffer.
   * The vblank wait is timeout-guarded so an unexpected status polarity
   * degrades to an immediate swap instead of hanging the UI task. */
  if (lv_display_flush_is_last(display))
  {
    uint32_t deadline = HAL_GetTick() + 20U;
    while ((hltdc.Instance->CDSR & LTDC_CDSR_VSYNCS) == 0U)
    {
      if ((int32_t)(HAL_GetTick() - deadline) >= 0)
      {
        break;
      }
      /* Yield while waiting for vblank: a busy spin here once ate the
       * timeslices the TCP stack needed during animations. */
      osDelay(1);
    }
    HAL_LTDC_SetAddress(&hltdc, (uint32_t)pixels, 0U);
  }
  lv_display_flush_ready(display);
}

static lv_color_t brand_color(const char *symbol)
{
  if (strcmp(symbol, "AMD") == 0) return lv_color_hex(0xED1C24);
  if (strcmp(symbol, "NVDA") == 0) return lv_color_hex(0x76B900);
  if (strcmp(symbol, "MSFT") == 0) return lv_color_hex(0x00A4EF);
  if (strcmp(symbol, "AAPL") == 0) return lv_color_hex(0xA3AAAD);
  if (strcmp(symbol, "TSLA") == 0) return lv_color_hex(0xCC0000);

  uint32_t hash = 2166136261U;
  while (*symbol != '\0')
  {
    hash ^= (uint8_t)*symbol++;
    hash *= 16777619U;
  }
  static const uint32_t palette[] = {
    0x60A5FA, 0xA78BFA, 0xF472B6, 0xFB923C, 0x34D399, 0xFBBF24
  };
  return lv_color_hex(palette[hash % (sizeof(palette) / sizeof(palette[0]))]);
}

static void create_badge(market_row_t *market)
{
  const lv_image_dsc_t *api_logo = logo_cache_lookup(market->symbol);
  if (api_logo != NULL)
  {
    market->badge = lv_image_create(market->row);
    lv_image_set_src(market->badge, api_logo);
    lv_image_set_scale(market->badge, 203);   /* 48px PNG -> 38px badge */
    lv_obj_set_size(market->badge, 38, 38);
    lv_obj_clear_flag(market->badge, LV_OBJ_FLAG_CLICKABLE);
    market->logo_applied = true;
    return;
  }

  if (strcmp(market->symbol, "AMD") == 0)
  {
    market->badge = lv_image_create(market->row);
    lv_image_set_src(market->badge, &logo_AMD);
    lv_image_set_scale(market->badge, 203);
    lv_obj_set_size(market->badge, 38, 38);
    lv_obj_clear_flag(market->badge, LV_OBJ_FLAG_CLICKABLE);
    market->logo_applied = true;
    return;
  }

  market->badge = lv_obj_create(market->row);
  lv_obj_set_size(market->badge, 38, 38);
  lv_obj_set_style_radius(market->badge, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(market->badge, brand_color(market->symbol), 0);
  lv_obj_set_style_border_width(market->badge, 0, 0);
  lv_obj_set_style_pad_all(market->badge, 0, 0);
  lv_obj_clear_flag(market->badge, LV_OBJ_FLAG_SCROLLABLE);

  char initial[2] = { market->symbol[0], '\0' };
  lv_obj_t *label = lv_label_create(market->badge);
  lv_label_set_text(label, initial);
  lv_obj_set_style_text_color(label, lv_color_hex(0x0B0F17), 0);
  lv_obj_set_style_text_font(label, &lv_font_montserrat_18, 0);
  lv_obj_center(label);
}

static void create_market_row(const char *symbol, market_row_t *market)
{
  memset(market, 0, sizeof(*market));
  snprintf(market->symbol, sizeof(market->symbol), "%s", symbol);

  market->row = lv_obj_create(list);
  lv_obj_set_size(market->row, LCD_WIDTH - 12, ROW_HEIGHT);
  lv_obj_set_style_bg_color(market->row, lv_color_hex(0x16202D), 0);
  lv_obj_set_style_bg_color(market->row, lv_color_hex(0x223349),
                            LV_STATE_PRESSED);
  lv_obj_set_style_border_width(market->row, 0, 0);
  lv_obj_set_style_radius(market->row, 8, 0);
  lv_obj_set_style_pad_hor(market->row, 8, 0);
  lv_obj_set_style_pad_ver(market->row, 7, 0);
  lv_obj_clear_flag(market->row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(market->row, row_clicked, LV_EVENT_CLICKED, market);

  create_badge(market);

  market->symbol_label = lv_label_create(market->row);
  lv_label_set_text(market->symbol_label, symbol);
  lv_obj_set_style_text_color(market->symbol_label, lv_color_hex(0xE7EEF7), 0);
  lv_obj_set_style_text_font(market->symbol_label, &lv_font_montserrat_18, 0);
  lv_obj_align(market->symbol_label, LV_ALIGN_LEFT_MID, 48, 0);

  market->spark = lv_line_create(market->row);
  lv_obj_set_size(market->spark, SPARK_WIDTH, SPARK_HEIGHT);
  lv_obj_align(market->spark, LV_ALIGN_CENTER, 22, 0);
  lv_obj_set_style_line_width(market->spark, 2, 0);
  lv_obj_set_style_line_rounded(market->spark, true, 0);
  lv_obj_set_style_line_color(market->spark, lv_color_hex(0x8A98AD), 0);

  market->price = lv_label_create(market->row);
  lv_label_set_text(market->price, "---.--");
  lv_obj_set_style_text_color(market->price, lv_color_hex(0xE7EEF7), 0);
  lv_obj_set_style_text_font(market->price, &lv_font_montserrat_18, 0);
  lv_obj_align(market->price, LV_ALIGN_TOP_RIGHT, 0, 0);

  market->change = lv_label_create(market->row);
  lv_label_set_text(market->change, "--.--%");
  lv_obj_set_style_text_color(market->change, lv_color_hex(0x8A98AD), 0);
  lv_obj_set_style_text_font(market->change, &lv_font_montserrat_14, 0);
  lv_obj_align(market->change, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
}

static void rebuild_rows(void)
{
  char symbols[APP_MAX_SYMBOLS][APP_SYMBOL_LENGTH];
  size_t count = settings_get_symbols(symbols);

  lv_obj_clean(list);
  row_count = count;
  for (size_t i = 0; i < row_count; ++i)
  {
    create_market_row(symbols[i], &rows[i]);
  }
  settings_seen = settings_generation();
}

static void update_spark(market_row_t *market, const stock_snapshot_t *snapshot,
                         lv_color_t color)
{
  if (snapshot->close_count < 2U)
  {
    lv_line_set_points(market->spark, market->points, 0);
    return;
  }

  float minimum = snapshot->closes[0];
  float maximum = snapshot->closes[0];
  for (size_t i = 1; i < snapshot->close_count; ++i)
  {
    if (snapshot->closes[i] < minimum) minimum = snapshot->closes[i];
    if (snapshot->closes[i] > maximum) maximum = snapshot->closes[i];
  }
  float span = maximum - minimum;
  if (span < 0.001f) span = 1.0f;

  for (size_t i = 0; i < snapshot->close_count; ++i)
  {
    market->points[i].x = (lv_value_precise_t)
        ((i * (SPARK_WIDTH - 2U)) / (snapshot->close_count - 1U));
    market->points[i].y = (lv_value_precise_t)
        ((maximum - snapshot->closes[i]) * (SPARK_HEIGHT - 4U) / span + 2.0f);
  }
  lv_line_set_points(market->spark, market->points, snapshot->close_count);
  lv_obj_set_style_line_color(market->spark, color, 0);
}

static void update_rows(void)
{
  stock_snapshot_t stocks[APP_MAX_SYMBOLS];
  size_t stock_count = stock_data_get_all(stocks);

  for (size_t row = 0; row < row_count; ++row)
  {
    /* Swap the placeholder badge for the real logo once it has been fetched. */
    if (!rows[row].logo_applied)
    {
      const lv_image_dsc_t *api_logo = logo_cache_lookup(rows[row].symbol);
      if (api_logo != NULL)
      {
        lv_obj_delete(rows[row].badge);
        rows[row].badge = lv_image_create(rows[row].row);
        lv_image_set_src(rows[row].badge, api_logo);
        lv_image_set_scale(rows[row].badge, 203);
        lv_obj_set_size(rows[row].badge, 38, 38);
        lv_obj_clear_flag(rows[row].badge, LV_OBJ_FLAG_CLICKABLE);
        rows[row].logo_applied = true;
        printf("[ui] logo applied: %s\r\n", rows[row].symbol);
      }
    }

    const stock_snapshot_t *snapshot = NULL;
    for (size_t i = 0; i < stock_count; ++i)
    {
      if (strcmp(rows[row].symbol, stocks[i].symbol) == 0)
      {
        snapshot = &stocks[i];
        break;
      }
    }
    if (snapshot == NULL || !snapshot->fresh) continue;

    char price[20], change[20], text[32];
    format_decimal_2(price, sizeof(price), snapshot->last, 0);
    format_decimal_2(change, sizeof(change), snapshot->change_pct, 1);
    lv_label_set_text(rows[row].price, price);
    snprintf(text, sizeof(text), "%s %s%%",
             snapshot->change_pct >= 0.0f ? LV_SYMBOL_UP : LV_SYMBOL_DOWN,
             change);
    lv_label_set_text(rows[row].change, text);
    lv_color_t accent = snapshot->change_pct >= 0.0f
        ? lv_color_hex(0x4ADE80) : lv_color_hex(0xF87171);
    lv_obj_set_style_text_color(rows[row].change, accent, 0);
    update_spark(&rows[row], snapshot, accent);
  }

  char updated[32];
  snprintf(updated, sizeof(updated), "updated %lus",
           (unsigned long)(HAL_GetTick() / 1000U));
  lv_label_set_text(updated_label, updated);
  update_detail();
}

/* Gradient under the curve. Drawn on LV_EVENT_DRAW_MAIN_BEGIN so the
 * chart's own line renders on top of the fill. */
static void chart_area_fill(lv_event_t *event)
{
  if (detail_fill_n < 2) return;
  lv_obj_t *chart = lv_event_get_target_obj(event);
  lv_layer_t *layer = lv_event_get_layer(event);
  if (chart == NULL || layer == NULL) return;
  lv_area_t coords;
  lv_obj_get_coords(chart, &coords);
  chart_util_draw_polyline_fill(layer, detail_fill_x, detail_fill_y,
                                detail_fill_n, coords.x1, coords.y1,
                                detail_fill_bottom, detail_line_color,
                                LV_OPA_70);
}

static float pick_y_step(float range)
{
  size_t count = sizeof(y_steps) / sizeof(y_steps[0]);
  if (range <= 0.0f) return 1.0f;
  for (size_t i = 0; i < count; ++i)
  {
    if (range / y_steps[i] <= 5.0f) return y_steps[i];
  }
  return y_steps[count - 1U];
}

static void render_history(const history_snapshot_t *history)
{
  size_t n = history->point_count;
  const float *closes = history->closes;
  if (n < 2U) return;

  lv_obj_add_flag(detail_spinner, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(detail_error_label, LV_OBJ_FLAG_HIDDEN);

  float lo = closes[0], hi = closes[0];
  for (size_t i = 1; i < n; ++i)
  {
    if (closes[i] < lo) lo = closes[i];
    if (closes[i] > hi) hi = closes[i];
  }
  if (hi - lo < 0.01f)
  {
    hi = lo + 0.5f;
    lo -= 0.5f;
  }

  /* Snap the Y range to "nice" tick values: 4-6 ticks at a clean step. */
  float step = pick_y_step(hi - lo);
  float lo_snap = floorf(lo / step) * step;
  float hi_snap = ceilf(hi / step) * step;
  if (hi_snap <= lo_snap) hi_snap = lo_snap + step;
  int n_y_ticks = (int)lroundf((hi_snap - lo_snap) / step) + 1;
  if (n_y_ticks < 2) n_y_ticks = 2;
  if (n_y_ticks > DETAIL_MAX_Y_TICKS) n_y_ticks = DETAIL_MAX_Y_TICKS;

  /* Pass 1: format + measure the Y labels so the gutter fits the widest. */
  char text[40];
  int widest = 0;
  int label_h = 16;
  for (int j = 0; j < DETAIL_MAX_Y_TICKS; ++j)
  {
    lv_obj_t *label = detail_y_labels[j];
    if (j >= n_y_ticks)
    {
      lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    float value = hi_snap - (float)j * step;
    if (step >= 1.0f) snprintf(text, sizeof(text), "%d", (int)lroundf(value));
    else format_decimal_2(text, sizeof(text), value, 0);
    lv_label_set_text(label, text);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_update_layout(label);
    int w = lv_obj_get_width(label);
    if (w > widest) widest = w;
    int h = lv_obj_get_height(label);
    if (h > label_h) label_h = h;
  }

  int gutter = widest + 6;
  int pad_bottom = label_h + 4;
  int plot_w = DETAIL_CHART_W - gutter;
  int plot_h = DETAIL_CHART_H - pad_bottom;
  if (plot_w < 1) plot_w = 1;
  if (plot_h < 1) plot_h = 1;

  /* The chart shrinks so the gutter and X strip live outside its box. */
  lv_obj_set_pos(detail_chart, gutter, 0);
  lv_obj_set_size(detail_chart, plot_w, plot_h);
  lv_chart_set_axis_range(detail_chart, LV_CHART_AXIS_PRIMARY_Y,
                          (int32_t)(lo_snap * 100.0f),
                          (int32_t)(hi_snap * 100.0f));

  /* Pass 2: place Y labels in the gutter, digits right-aligned. */
  for (int j = 0; j < n_y_ticks; ++j)
  {
    int lh = lv_obj_get_height(detail_y_labels[j]);
    int lw = lv_obj_get_width(detail_y_labels[j]);
    int ly = (plot_h * j) / (n_y_ticks - 1) - lh / 2;
    if (ly < 0) ly = 0;
    if (ly + lh > plot_h) ly = plot_h - lh;
    int lx = (gutter - 4) - lw;
    if (lx < 0) lx = 0;
    lv_obj_set_pos(detail_y_labels[j], lx, ly);
  }

  /* Monotone cubic smoothing — no Catmull-Rom overshoot on noisy closes. */
  static float smooth[DETAIL_CR_MAX_OUT];
  int out_n = ((int)n - 1) * DETAIL_CR_FACTOR + 1;
  if (out_n > (int)DETAIL_CR_MAX_OUT) out_n = (int)DETAIL_CR_MAX_OUT;
  chart_util_monotone_cubic(closes, n, smooth, (size_t)out_n,
                            DETAIL_CR_FACTOR);

  float span = hi_snap - lo_snap;
  if (span <= 0.0f) span = 1.0f;

  /* Progressive 1D: the X axis spans the WHOLE trading session and the
   * line fills only the elapsed part (Revolut-style). Other ranges fill
   * the full width. */
  bool progressive = strcmp(history->range, "1d") == 0;
  static float slot_value[DETAIL_CR_MAX_OUT];
  int total_slots, active_n;
  uint32_t session_open = 0, session_close = 0;
  if (progressive)
  {
    session_open = history->session_open;
    session_close = history->session_close;
    uint32_t first_ts = history->timestamps[0];
    uint32_t last_ts = history->timestamps[n - 1U];
    if (session_open == 0U || session_close <= session_open)
    {
      /* API omitted session bounds: assume a 6.5 h regular US session
       * starting at the first data point. */
      session_open = first_ts != 0U ? first_ts : last_ts;
      session_close = session_open + 23400U;
    }
    float fraction = 1.0f;
    if (session_close > session_open && last_ts > 0U)
    {
      fraction = (float)(last_ts - session_open) /
                 (float)(session_close - session_open);
    }
    if (fraction < 0.02f) fraction = 0.02f;   /* always show a sliver */
    if (fraction > 1.0f) fraction = 1.0f;

    total_slots = (int)DETAIL_CR_MAX_OUT;
    active_n = (int)lroundf(fraction * (float)(total_slots - 1)) + 1;
    if (active_n < 2) active_n = 2;
    if (active_n > total_slots) active_n = total_slots;

    /* Resample the dense smoothed curve into the elapsed slots. */
    for (int i = 0; i < active_n; ++i)
    {
      float pos = (active_n > 1)
          ? (float)i / (float)(active_n - 1) * (float)(out_n - 1) : 0.0f;
      int i0 = (int)pos;
      int i1 = (i0 + 1 < out_n) ? i0 + 1 : i0;
      float fr = pos - (float)i0;
      slot_value[i] = smooth[i0] * (1.0f - fr) + smooth[i1] * fr;
    }
  }
  else
  {
    total_slots = out_n;
    active_n = out_n;
    for (int i = 0; i < out_n; ++i) slot_value[i] = smooth[i];
  }

  lv_chart_set_point_count(detail_chart, (uint32_t)total_slots);
  for (int i = 0; i < total_slots; ++i)
  {
    lv_chart_set_series_value_by_id(
        detail_chart, detail_series, (uint32_t)i,
        i < active_n ? (int32_t)(slot_value[i] * 100.0f)
                     : LV_CHART_POINT_NONE);
  }

  /* Fill polygon geometry (chart-local px) for the gradient callback. */
  detail_fill_n = active_n;
  detail_fill_bottom = plot_h;
  for (int i = 0; i < active_n; ++i)
  {
    detail_fill_x[i] = (plot_w * i) / (total_slots - 1);
    float ynorm = 1.0f - (slot_value[i] - lo_snap) / span;
    int y = (int)lroundf(ynorm * (float)plot_h);
    if (y < 0) y = 0;
    if (y > plot_h) y = plot_h;
    detail_fill_y[i] = y;
  }

  lv_chart_refresh(detail_chart);

  /* Header reflects the displayed window: last close + window % change. */
  float first = closes[0];
  float last = closes[n - 1U];
  float change = first != 0.0f ? (last - first) / first * 100.0f : 0.0f;
  bool up = change >= 0.0f;

  char number[20];
  format_decimal_2(number, sizeof(number), last, 0);
  lv_label_set_text(detail_price, number);
  format_decimal_2(number, sizeof(number), change, 1);
  snprintf(text, sizeof(text), "%s %s%%",
           up ? LV_SYMBOL_UP : LV_SYMBOL_DOWN, number);
  lv_label_set_text(detail_change, text);

  lv_color_t accent = up ? lv_color_hex(0x4ADE80) : lv_color_hex(0xF87171);
  lv_obj_set_style_text_color(detail_change, accent, 0);
  detail_line_color = accent;
  lv_chart_set_series_color(detail_chart, detail_series, accent);

  /* X-axis labels reflect the requested range, not the API interval.
   * 1d -> fixed session ticks (open..close clock times) so the axis covers
   * the whole day regardless of elapsed time; other ranges sample dates
   * from the points, last tick anchored to the newest point. */
  if (progressive)
  {
    uint32_t session_span = session_close - session_open;
    for (int k = 0; k < DETAIL_X_TICKS; ++k)
    {
      uint32_t t = session_open +
          (uint32_t)(((uint64_t)session_span * (uint32_t)k) /
                     (uint32_t)(DETAIL_X_TICKS - 1));
      chart_civil_t civil;
      chart_util_epoch_to_civil(t, APP_UTC_OFFSET_MINUTES, &civil);
      snprintf(text, sizeof(text), "%02d:%02d", civil.hour, civil.minute);
      lv_label_set_text(detail_x_labels[k], text);
    }
  }
  else
  {
    /* Pick the date format from the window's wall-time span: years-wide
     * windows label years, medium ones month+year, short ones day+month. */
    long span_days = 0;
    if (history->timestamps[0] > 0U &&
        history->timestamps[n - 1U] > history->timestamps[0])
    {
      span_days = (long)((history->timestamps[n - 1U] -
                          history->timestamps[0]) / 86400U);
    }
    for (int k = 0; k < DETAIL_X_TICKS; ++k)
    {
      size_t index = ((n - 1U) * (size_t)k) / (size_t)(DETAIL_X_TICKS - 1);
      uint32_t t = history->timestamps[index];
      if (t == 0U)
      {
        lv_label_set_text(detail_x_labels[k], "");
        continue;
      }
      chart_civil_t civil;
      chart_util_epoch_to_civil(t, APP_UTC_OFFSET_MINUTES, &civil);
      if (span_days > 730)
      {
        snprintf(text, sizeof(text), "%d", civil.year);
      }
      else if (span_days > 365)
      {
        snprintf(text, sizeof(text), "%s %02d",
                 month_names[civil.month - 1], civil.year % 100);
      }
      else
      {
        snprintf(text, sizeof(text), "%02d %s", civil.day,
                 month_names[civil.month - 1]);
      }
      lv_label_set_text(detail_x_labels[k], text);
    }
  }
  /* Place the X labels under the plot, centered on their tick. */
  for (int k = 0; k < DETAIL_X_TICKS; ++k)
  {
    lv_obj_t *label = detail_x_labels[k];
    int x_px = gutter + (plot_w * k) / (DETAIL_X_TICKS - 1);
    lv_obj_update_layout(label);
    int lw = lv_obj_get_width(label);
    int lh = lv_obj_get_height(label);
    int lx = x_px - lw / 2;
    if (lx < 0) lx = 0;
    if (lx + lw > DETAIL_CHART_W) lx = DETAIL_CHART_W - lw;
    int ly = plot_h + (pad_bottom - lh) / 2;
    if (ly < plot_h) ly = plot_h;
    lv_obj_set_pos(label, lx, ly);
  }

  /* Current-price marker dot at the line's right end. */
  lv_obj_set_style_bg_color(detail_marker, accent, 0);
  lv_obj_clear_flag(detail_marker, LV_OBJ_FLAG_HIDDEN);
  lv_obj_update_layout(detail_chart);
  lv_point_t tip;
  lv_chart_get_point_pos_by_id(detail_chart, detail_series,
                               (uint32_t)(active_n - 1), &tip);
  int dot_x = tip.x - DETAIL_MARKER_SIZE / 2;
  int dot_y = tip.y - DETAIL_MARKER_SIZE / 2;
  if (dot_x < 0) dot_x = 0;
  if (dot_x > plot_w - DETAIL_MARKER_SIZE) dot_x = plot_w - DETAIL_MARKER_SIZE;
  if (dot_y < 0) dot_y = 0;
  if (dot_y > plot_h - DETAIL_MARKER_SIZE) dot_y = plot_h - DETAIL_MARKER_SIZE;
  lv_obj_set_pos(detail_marker, dot_x, dot_y);

  /* New data is on screen: the highlighted button now matches the chart. */
  detail_window_rendered = true;
  detail_range_displayed = detail_range_pending;
  apply_range_styles();
}

static void update_detail(void)
{
  if (detail_screen == NULL) return;

  /* Live quote keeps the header price fresh; the day-change % shows only
   * until a history window renders (then the window % owns the label). */
  stock_snapshot_t stocks[APP_MAX_SYMBOLS];
  size_t stock_count = stock_data_get_all(stocks);
  for (size_t i = 0; i < stock_count; ++i)
  {
    if (!stocks[i].fresh || strcmp(stocks[i].symbol, detail_symbol) != 0)
    {
      continue;
    }
    char price[20];
    format_decimal_2(price, sizeof(price), stocks[i].last, 0);
    lv_label_set_text(detail_price, price);
    if (!detail_window_rendered)
    {
      char pct[20], text[32];
      format_decimal_2(pct, sizeof(pct), stocks[i].change_pct, 1);
      snprintf(text, sizeof(text), "%s %s%%",
               stocks[i].change_pct >= 0.0f ? LV_SYMBOL_UP : LV_SYMBOL_DOWN,
               pct);
      lv_label_set_text(detail_change, text);
      lv_obj_set_style_text_color(detail_change,
                                  stocks[i].change_pct >= 0.0f
                                      ? lv_color_hex(0x4ADE80)
                                      : lv_color_hex(0xF87171), 0);
    }
    break;
  }

  history_snapshot_t history;
  if (!history_data_get(&history)) return;
  if (history.generation != detail_history_generation) return;
  if (strcmp(history.symbol, detail_symbol) != 0) return;
  if (detail_done_generation == history.generation) return;

  if (history.error)
  {
    detail_done_generation = history.generation;
    lv_obj_add_flag(detail_spinner, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(detail_error_label,
                      history.status[0] != '\0' ? history.status : "no data");
    lv_obj_clear_flag(detail_error_label, LV_OBJ_FLAG_HIDDEN);
    /* Revert the highlight to the still-displayed range; a retap of the
     * failed button re-issues the fetch. */
    detail_range_pending = detail_range_displayed;
    apply_range_styles();
    return;
  }
  if (!history.fresh) return;

  detail_done_generation = history.generation;
  render_history(&history);
  printf("[ui] %s %s rendered: %u points\r\n", history.symbol, history.range,
         (unsigned)history.point_count);
}

static void create_market_screen(void)
{
  lv_obj_t *screen = lv_screen_active();
  market_screen = screen;
  lv_obj_set_style_bg_color(screen, lv_color_hex(0x0B0F17), 0);
  lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(screen, 0, 0);

  lv_obj_t *bar = lv_obj_create(screen);
  lv_obj_set_size(bar, LCD_WIDTH, STATUS_HEIGHT);
  lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
  lv_obj_set_style_bg_color(bar, lv_color_hex(0x07090D), 0);
  lv_obj_set_style_border_width(bar, 0, 0);
  lv_obj_set_style_radius(bar, 0, 0);
  lv_obj_set_style_pad_hor(bar, 8, 0);
  lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

  link_label = lv_label_create(bar);
  lv_label_set_text(link_label, LV_SYMBOL_WIFI " Ethernet");
  lv_obj_set_style_text_color(link_label, lv_color_hex(0x4ADE80), 0);
  lv_obj_align(link_label, LV_ALIGN_LEFT_MID, 0, 0);

  lv_obj_t *title = lv_label_create(bar);
  lv_label_set_text(title, "MARKETS");
  lv_obj_set_style_text_color(title, lv_color_hex(0xE7EEF7), 0);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
  lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);

  updated_label = lv_label_create(bar);
  lv_label_set_text(updated_label, "starting...");
  lv_obj_set_style_text_color(updated_label, lv_color_hex(0x8A98AD), 0);
  lv_obj_align(updated_label, LV_ALIGN_RIGHT_MID, 0, 0);

  list = lv_obj_create(screen);
  lv_obj_set_size(list, LCD_WIDTH, LCD_HEIGHT - STATUS_HEIGHT);
  lv_obj_align(list, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(list, ROW_GAP, 0);
  lv_obj_set_style_pad_ver(list, ROW_GAP, 0);
  lv_obj_set_style_bg_color(list, lv_color_hex(0x0B0F17), 0);
  lv_obj_set_style_border_width(list, 0, 0);
  lv_obj_set_style_radius(list, 0, 0);
  lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
  lv_obj_set_style_bg_color(list, lv_color_hex(0xED1C24), LV_PART_SCROLLBAR);
  lv_obj_set_style_width(list, 4, LV_PART_SCROLLBAR);

  rebuild_rows();
}

static lv_obj_t *create_text_button(lv_obj_t *parent, const char *text,
                                    lv_event_cb_t callback, void *user_data)
{
  lv_obj_t *button = lv_button_create(parent);
  lv_obj_set_size(button, 48, 26);
  lv_obj_set_style_bg_color(button, lv_color_hex(0x263244), 0);
  lv_obj_set_style_bg_color(button, lv_color_hex(0xED1C24), LV_STATE_PRESSED);
  lv_obj_set_style_radius(button, 6, 0);
  lv_obj_set_style_pad_all(button, 0, 0);
  lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, user_data);

  lv_obj_t *label = lv_label_create(button);
  lv_label_set_text(label, text);
  lv_obj_set_style_text_color(label, lv_color_hex(0xE7EEF7), 0);
  lv_obj_center(label);
  return button;
}

static void create_detail_screen(const char *symbol)
{
  snprintf(detail_symbol, sizeof(detail_symbol), "%s", symbol);
  detail_window_rendered = false;
  detail_range_displayed = 0;
  detail_range_pending = 0;
  detail_fill_n = 0;
  detail_line_color = lv_color_hex(0x4ADE80);

  detail_screen = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(detail_screen, lv_color_hex(0x0B0F17), 0);
  lv_obj_set_style_bg_opa(detail_screen, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(detail_screen, DETAIL_PAD, 0);
  /* Everything fits in 480x272 - never scroll the detail screen. */
  lv_obj_clear_flag(detail_screen, LV_OBJ_FLAG_SCROLLABLE);

  /* Compact single-row header:
   * [logo] [symbol] [price] [chg %] ...spacer... [back] */
  lv_obj_t *header = lv_obj_create(detail_screen);
  lv_obj_remove_style_all(header);
  lv_obj_set_size(header, DETAIL_CONTENT_W, DETAIL_HEADER_H);
  lv_obj_align(header, LV_ALIGN_TOP_LEFT, 0, 0);
  lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(header, 8, 0);
  lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

  /* Prefer the fetched logo; fall back to the bundled AMD asset, then to a
   * brand-colored initial badge (mirrors create_badge() for the rows). */
  const lv_image_dsc_t *api_logo = logo_cache_lookup(symbol);
  if (api_logo != NULL || strcmp(symbol, "AMD") == 0)
  {
    lv_obj_t *logo = lv_image_create(header);
    lv_image_set_src(logo, api_logo != NULL ? api_logo : &logo_AMD);
    lv_image_set_scale(logo, 213);   /* 48px source -> 40px slot */
    lv_obj_set_size(logo, 40, 40);
  }
  else
  {
    lv_obj_t *badge = lv_obj_create(header);
    lv_obj_set_size(badge, 40, 40);
    lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(badge, brand_color(symbol), 0);
    lv_obj_set_style_border_width(badge, 0, 0);
    lv_obj_set_style_pad_all(badge, 0, 0);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE);

    char initial[2] = { symbol[0], '\0' };
    lv_obj_t *badge_label = lv_label_create(badge);
    lv_label_set_text(badge_label, initial);
    lv_obj_set_style_text_color(badge_label, lv_color_hex(0x0B0F17), 0);
    lv_obj_set_style_text_font(badge_label, &lv_font_montserrat_20, 0);
    lv_obj_center(badge_label);
  }

  lv_obj_t *symbol_label = lv_label_create(header);
  lv_label_set_text(symbol_label, symbol);
  lv_obj_set_style_text_color(symbol_label, lv_color_hex(0xE7EEF7), 0);
  lv_obj_set_style_text_font(symbol_label, &lv_font_montserrat_20, 0);

  detail_price = lv_label_create(header);
  lv_label_set_text(detail_price, "---.--");
  lv_obj_set_style_text_color(detail_price, lv_color_hex(0xE7EEF7), 0);
  lv_obj_set_style_text_font(detail_price, &lv_font_montserrat_20, 0);

  detail_change = lv_label_create(header);
  lv_label_set_text(detail_change, "--.--%");
  lv_obj_set_style_text_color(detail_change, lv_color_hex(0x8A98AD), 0);
  lv_obj_set_style_text_font(detail_change, &lv_font_montserrat_14, 0);

  /* Flex-grow spacer pushes the back button to the far right. */
  lv_obj_t *spacer = lv_obj_create(header);
  lv_obj_remove_style_all(spacer);
  lv_obj_set_height(spacer, 1);
  lv_obj_set_flex_grow(spacer, 1);

  lv_obj_t *back = create_text_button(header, LV_SYMBOL_LEFT, back_clicked,
                                      NULL);
  lv_obj_set_size(back, 44, DETAIL_HEADER_H - 8);
  lv_obj_set_ext_click_area(back, 8);

  /* Range buttons above the chart; index baked into the event user_data. */
  lv_obj_t *range_row = lv_obj_create(detail_screen);
  lv_obj_remove_style_all(range_row);
  lv_obj_set_size(range_row, DETAIL_CONTENT_W, DETAIL_BTN_ROW_H);
  lv_obj_align(range_row, LV_ALIGN_TOP_LEFT, 0, DETAIL_HEADER_H + 2);
  lv_obj_set_flex_flow(range_row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(range_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(range_row, LV_OBJ_FLAG_SCROLLABLE);

  const int button_w =
      (DETAIL_CONTENT_W - 4 * (DETAIL_NUM_RANGES - 1)) / DETAIL_NUM_RANGES;
  for (int i = 0; i < DETAIL_NUM_RANGES; ++i)
  {
    lv_obj_t *button = lv_button_create(range_row);
    lv_obj_remove_style_all(button);
    lv_obj_set_size(button, button_w, DETAIL_BTN_ROW_H);
    lv_obj_set_style_radius(button, 5, 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(button, &lv_font_montserrat_14, 0);

    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, range_labels[i]);
    lv_obj_center(label);

    lv_obj_add_event_cb(button, range_clicked, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)i);
    detail_range_buttons[i] = button;
  }

  /* Card holding the chart, its Y-label gutter and X-label strip. */
  detail_card = lv_obj_create(detail_screen);
  lv_obj_set_size(detail_card, DETAIL_CONTENT_W, DETAIL_CARD_H);
  lv_obj_align(detail_card, LV_ALIGN_TOP_LEFT, 0, DETAIL_CARD_Y);
  lv_obj_set_style_bg_color(detail_card, lv_color_hex(0x121A25), 0);
  lv_obj_set_style_bg_opa(detail_card, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(detail_card, lv_color_hex(0x263244), 0);
  lv_obj_set_style_border_width(detail_card, 1, 0);
  lv_obj_set_style_radius(detail_card, 10, 0);
  lv_obj_set_style_pad_all(detail_card, DETAIL_CARD_PAD, 0);
  lv_obj_clear_flag(detail_card, LV_OBJ_FLAG_SCROLLABLE);

  /* Chart fills the card initially; render_history() shrinks it so the
   * Y gutter (left) and X strip (below) sit outside its bounding box. */
  detail_chart = lv_chart_create(detail_card);
  lv_obj_set_size(detail_chart, DETAIL_CHART_W, DETAIL_CHART_H);
  lv_obj_set_pos(detail_chart, 0, 0);
  lv_chart_set_type(detail_chart, LV_CHART_TYPE_LINE);
  lv_chart_set_point_count(detail_chart, DETAIL_CR_MAX_OUT);
  lv_chart_set_div_line_count(detail_chart, 5, 0);
  lv_obj_set_style_pad_all(detail_chart, 0, LV_PART_MAIN);
  lv_obj_set_style_size(detail_chart, 0, 0, LV_PART_INDICATOR);
  lv_obj_set_style_line_width(detail_chart, 3, LV_PART_ITEMS);
  lv_obj_set_style_bg_opa(detail_chart, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(detail_chart, 0, 0);
  lv_obj_set_style_line_color(detail_chart, lv_color_hex(0x263244),
                              LV_PART_MAIN);
  lv_obj_set_style_line_opa(detail_chart, LV_OPA_40, LV_PART_MAIN);
  lv_obj_add_event_cb(detail_chart, chart_area_fill,
                      LV_EVENT_DRAW_MAIN_BEGIN, NULL);
  detail_series = lv_chart_add_series(detail_chart, detail_line_color,
                                      LV_CHART_AXIS_PRIMARY_Y);

  /* Round loading indicator, centered over the (blank) chart. */
  detail_spinner = lv_spinner_create(detail_card);
  lv_obj_set_size(detail_spinner, 48, 48);
  lv_obj_center(detail_spinner);
  lv_obj_set_style_arc_width(detail_spinner, 5, LV_PART_MAIN);
  lv_obj_set_style_arc_width(detail_spinner, 5, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(detail_spinner, lv_color_hex(0x263244),
                             LV_PART_MAIN);
  lv_obj_set_style_arc_color(detail_spinner, lv_color_hex(0xED1C24),
                             LV_PART_INDICATOR);

  detail_error_label = lv_label_create(detail_card);
  lv_label_set_text(detail_error_label, "no data");
  lv_obj_set_style_text_color(detail_error_label, lv_color_hex(0xF87171), 0);
  lv_obj_set_style_text_font(detail_error_label, &lv_font_montserrat_16, 0);
  lv_obj_center(detail_error_label);
  lv_obj_add_flag(detail_error_label, LV_OBJ_FLAG_HIDDEN);

  /* Tick label pools; render_history() formats, shows and places them. */
  for (int j = 0; j < DETAIL_MAX_Y_TICKS; ++j)
  {
    lv_obj_t *label = lv_label_create(detail_card);
    lv_label_set_text(label, "");
    lv_obj_set_style_text_color(label, lv_color_hex(0x8A98AD), 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
    lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
    detail_y_labels[j] = label;
  }
  for (int k = 0; k < DETAIL_X_TICKS; ++k)
  {
    lv_obj_t *label = lv_label_create(detail_card);
    lv_label_set_text(label, "");
    lv_obj_set_style_text_color(label, lv_color_hex(0x8A98AD), 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
    detail_x_labels[k] = label;
  }

  /* Current-price marker; chart child so chart-local coords apply. */
  detail_marker = lv_obj_create(detail_chart);
  lv_obj_remove_style_all(detail_marker);
  lv_obj_set_size(detail_marker, DETAIL_MARKER_SIZE, DETAIL_MARKER_SIZE);
  lv_obj_set_style_radius(detail_marker, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(detail_marker, LV_OPA_COVER, 0);
  lv_obj_clear_flag(detail_marker, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(detail_marker, LV_OBJ_FLAG_HIDDEN);

  apply_range_styles();
  update_detail();
  lv_screen_load(detail_screen);
  start_history_fetch();
}

void StartUiTask(void const *argument)
{
  (void)argument;

  printf("[ui] initializing LVGL\r\n");
  lv_init();
  lv_tick_set_cb(HAL_GetTick);
  printf("[ui] creating display\r\n");
  lv_display_t *display = lv_display_create(LCD_WIDTH, LCD_HEIGHT);
  lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
  /* Clear the second framebuffer so the first flip can't show garbage. */
  printf("[ui] clearing second framebuffer\r\n");
  memset(FRAMEBUFFER_1, 0, FRAMEBUFFER_SIZE);
  printf("[ui] configuring display buffers\r\n");
  lv_display_set_buffers(display, FRAMEBUFFER_0, FRAMEBUFFER_1,
                         FRAMEBUFFER_SIZE, LV_DISPLAY_RENDER_MODE_DIRECT);
  lv_display_set_flush_cb(display, display_flush);

  printf("[ui] creating market screen\r\n");
  create_market_screen();
  touch_ft5336_init();
  printf("[display] CYD-style markets list active\r\n");

  uint32_t last_update = 0;
  for (;;)
  {
    if (settings_generation() != settings_seen) rebuild_rows();
    if (HAL_GetTick() - last_update >= 500U)
    {
      update_rows();
      last_update = HAL_GetTick();
    }

    uint32_t delay_ms = lv_timer_handler();
    if (delay_ms < 5U) delay_ms = 5U;
    if (delay_ms > 20U) delay_ms = 20U;
    osDelay(delay_ms);
  }
}
