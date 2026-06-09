#include "app/ui_task.h"
#include "app/config.h"
#include "app/format.h"
#include "app/history_data.h"
#include "app/logo_cache.h"
#include "app/logos.h"
#include "app/settings.h"
#include "app/stock_data.h"
#include "app/touch_ft5336.h"

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
#define DETAIL_CHART_WIDTH (LCD_WIDTH - 32)
#define DETAIL_CHART_HEIGHT 128

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
static lv_obj_t *detail_price;
static lv_obj_t *detail_change;
static lv_obj_t *detail_chart;
static lv_obj_t *detail_status;
static lv_obj_t *selected_range_button;
static char detail_symbol[APP_SYMBOL_LENGTH];
static lv_point_precise_t detail_points[STOCK_SPARKLINE_MAX_POINTS];
static uint32_t detail_history_generation;

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
  detail_chart = NULL;
  detail_status = NULL;
  selected_range_button = NULL;
}

static void range_clicked(lv_event_t *event)
{
  lv_obj_t *button = lv_event_get_target_obj(event);
  const char *range = lv_event_get_user_data(event);
  if (selected_range_button != NULL)
  {
    lv_obj_set_style_bg_color(selected_range_button, lv_color_hex(0x263244), 0);
  }
  selected_range_button = button;
  lv_obj_set_style_bg_color(button, lv_color_hex(0xED1C24), 0);

  char text[40];
  snprintf(text, sizeof(text), "Loading %s history...", range);
  lv_label_set_text(detail_status, text);
  detail_history_generation = history_data_request(detail_symbol, range);
  printf("[ui] history range requested: %s\r\n", range);
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

static void update_detail(void)
{
  if (detail_screen == NULL) return;

  stock_snapshot_t stocks[APP_MAX_SYMBOLS];
  size_t stock_count = stock_data_get_all(stocks);
  const stock_snapshot_t *snapshot = NULL;
  for (size_t i = 0; i < stock_count; ++i)
  {
    if (stocks[i].fresh && strcmp(stocks[i].symbol, detail_symbol) == 0)
    {
      snapshot = &stocks[i];
      break;
    }
  }
  if (snapshot == NULL)
  {
    return;
  }

  char price[20], change[20], text[32];
  format_decimal_2(price, sizeof(price), snapshot->last, 0);
  format_decimal_2(change, sizeof(change), snapshot->change_pct, 1);
  lv_label_set_text(detail_price, price);
  snprintf(text, sizeof(text), "%s %s%%",
           snapshot->change_pct >= 0.0f ? LV_SYMBOL_UP : LV_SYMBOL_DOWN,
           change);
  lv_label_set_text(detail_change, text);
  lv_color_t accent = snapshot->change_pct >= 0.0f
      ? lv_color_hex(0x4ADE80) : lv_color_hex(0xF87171);
  lv_obj_set_style_text_color(detail_change, accent, 0);

  const float *closes = snapshot->closes;
  size_t point_count = snapshot->close_count;
  history_snapshot_t history;
  if (history_data_get(&history) &&
      history.generation == detail_history_generation &&
      strcmp(history.symbol, detail_symbol) == 0)
  {
    if (history.error)
    {
      lv_label_set_text(detail_status, history.status);
    }
    else if (history.fresh)
    {
      closes = history.closes;
      point_count = history.point_count;
      float period_change = (closes[point_count - 1U] - closes[0]) /
                            closes[0] * 100.0f;
      format_decimal_2(change, sizeof(change), period_change, 1);
      snprintf(text, sizeof(text), "%s %s%%",
               period_change >= 0.0f ? LV_SYMBOL_UP : LV_SYMBOL_DOWN, change);
      lv_label_set_text(detail_change, text);
      accent = period_change >= 0.0f
          ? lv_color_hex(0x4ADE80) : lv_color_hex(0xF87171);
      lv_obj_set_style_text_color(detail_change, accent, 0);
      snprintf(text, sizeof(text), "%s: %u points", history.range,
               (unsigned)history.point_count);
      lv_label_set_text(detail_status, text);
    }
  }

  if (point_count < 2U)
  {
    lv_line_set_points(detail_chart, detail_points, 0);
    return;
  }

  float minimum = closes[0];
  float maximum = closes[0];
  for (size_t i = 1; i < point_count; ++i)
  {
    if (closes[i] < minimum) minimum = closes[i];
    if (closes[i] > maximum) maximum = closes[i];
  }
  float span = maximum - minimum;
  if (span < 0.001f) span = 1.0f;
  for (size_t i = 0; i < point_count; ++i)
  {
    detail_points[i].x = (lv_value_precise_t)
        ((i * (DETAIL_CHART_WIDTH - 2U)) / (point_count - 1U));
    detail_points[i].y = (lv_value_precise_t)
        ((maximum - closes[i]) * (DETAIL_CHART_HEIGHT - 8U) / span + 4.0f);
  }
  lv_line_set_points(detail_chart, detail_points, point_count);
  lv_obj_set_style_line_color(detail_chart, accent, 0);
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
  detail_screen = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(detail_screen, lv_color_hex(0x0B0F17), 0);
  lv_obj_set_style_bg_opa(detail_screen, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(detail_screen, 8, 0);
  /* Everything fits in 480x272 - never scroll the detail screen. */
  lv_obj_clear_flag(detail_screen, LV_OBJ_FLAG_SCROLLABLE);

  /* Prefer the fetched logo; fall back to the bundled AMD asset, then to a
   * brand-colored initial badge (mirrors create_badge() for the market rows). */
  const lv_image_dsc_t *api_logo = logo_cache_lookup(symbol);
  if (api_logo != NULL)
  {
    lv_obj_t *logo = lv_image_create(detail_screen);
    lv_image_set_src(logo, api_logo);
    lv_obj_align(logo, LV_ALIGN_TOP_LEFT, 0, 0);
  }
  else if (strcmp(symbol, "AMD") == 0)
  {
    lv_obj_t *logo = lv_image_create(detail_screen);
    lv_image_set_src(logo, &logo_AMD);
    lv_obj_align(logo, LV_ALIGN_TOP_LEFT, 0, 0);
  }
  else
  {
    lv_obj_t *badge = lv_obj_create(detail_screen);
    lv_obj_set_size(badge, 48, 48);
    lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(badge, brand_color(symbol), 0);
    lv_obj_set_style_border_width(badge, 0, 0);
    lv_obj_set_style_pad_all(badge, 0, 0);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(badge, LV_ALIGN_TOP_LEFT, 0, 0);

    char initial[2] = { symbol[0], '\0' };
    lv_obj_t *badge_label = lv_label_create(badge);
    lv_label_set_text(badge_label, initial);
    lv_obj_set_style_text_color(badge_label, lv_color_hex(0x0B0F17), 0);
    lv_obj_set_style_text_font(badge_label, &lv_font_montserrat_20, 0);
    lv_obj_center(badge_label);
  }

  lv_obj_t *symbol_label = lv_label_create(detail_screen);
  lv_label_set_text(symbol_label, symbol);
  lv_obj_set_style_text_color(symbol_label, lv_color_hex(0xE7EEF7), 0);
  lv_obj_set_style_text_font(symbol_label, &lv_font_montserrat_20, 0);
  lv_obj_align(symbol_label, LV_ALIGN_TOP_LEFT, 58, 2);

  detail_price = lv_label_create(detail_screen);
  lv_label_set_text(detail_price, "---.--");
  lv_obj_set_style_text_color(detail_price, lv_color_hex(0xE7EEF7), 0);
  lv_obj_set_style_text_font(detail_price, &lv_font_montserrat_20, 0);
  lv_obj_align(detail_price, LV_ALIGN_TOP_LEFT, 58, 27);

  detail_change = lv_label_create(detail_screen);
  lv_label_set_text(detail_change, "--.--%");
  lv_obj_set_style_text_color(detail_change, lv_color_hex(0x8A98AD), 0);
  lv_obj_align(detail_change, LV_ALIGN_TOP_LEFT, 154, 31);

  create_text_button(detail_screen, LV_SYMBOL_LEFT " Back", back_clicked, NULL);
  lv_obj_align(lv_obj_get_child(detail_screen, -1), LV_ALIGN_TOP_RIGHT, 0, 4);

  detail_chart = lv_line_create(detail_screen);
  lv_obj_set_size(detail_chart, DETAIL_CHART_WIDTH, DETAIL_CHART_HEIGHT);
  lv_obj_align(detail_chart, LV_ALIGN_TOP_MID, 0, 58);
  lv_obj_set_style_bg_color(detail_chart, lv_color_hex(0x121A25), 0);
  lv_obj_set_style_bg_opa(detail_chart, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(detail_chart, lv_color_hex(0x263244), 0);
  lv_obj_set_style_border_width(detail_chart, 1, 0);
  lv_obj_set_style_line_width(detail_chart, 3, 0);
  lv_obj_set_style_line_rounded(detail_chart, true, 0);

  static const char *range_labels[] = {
    "1D", "1W", "1M", "6M", "1Y", "5Y", "Max"
  };
  static const char *range_api[] = {
    "1d", "1w", "1mo", "6mo", "1y", "5y", "max"
  };
  lv_obj_t *range_row = lv_obj_create(detail_screen);
  lv_obj_set_size(range_row, LCD_WIDTH - 16, 30);
  lv_obj_align(range_row, LV_ALIGN_BOTTOM_MID, 0, -28);
  lv_obj_set_flex_flow(range_row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(range_row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_bg_opa(range_row, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(range_row, 0, 0);
  lv_obj_set_style_pad_all(range_row, 0, 0);
  lv_obj_clear_flag(range_row, LV_OBJ_FLAG_SCROLLABLE);
  for (size_t i = 0; i < sizeof(range_labels) / sizeof(range_labels[0]); ++i)
  {
    lv_obj_t *button = create_text_button(range_row, range_labels[i],
                                          range_clicked, (void *)range_api[i]);
    if (i == 0U)
    {
      selected_range_button = button;
      lv_obj_set_style_bg_color(button, lv_color_hex(0xED1C24), 0);
    }
  }

  detail_status = lv_label_create(detail_screen);
  lv_label_set_text(detail_status, "Live quote sparkline");
  lv_obj_set_style_text_color(detail_status, lv_color_hex(0x8A98AD), 0);
  lv_obj_align(detail_status, LV_ALIGN_BOTTOM_MID, 0, -6);

  update_detail();
  lv_screen_load(detail_screen);
  detail_history_generation = history_data_request(detail_symbol, "1d");
  lv_label_set_text(detail_status, "Loading 1d history...");
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
