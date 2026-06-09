#include "app/touch_ft5336.h"
#include "app/config.h"

#include <stdio.h>

#include "lvgl.h"
#include "main.h"

#define FT5336_ADDRESS       0x70U
#define FT5336_TD_STAT_REG   0x02U
#define FT5336_CHIP_ID_REG   0xA8U
#define FT5336_CHIP_ID       0x51U

extern I2C_HandleTypeDef hi2c3;

static bool touch_ready;
static bool touch_pressed;

static void touch_read(lv_indev_t *indev, lv_indev_data_t *data)
{
  (void)indev;
  uint8_t registers[5];

  if (!touch_ready ||
      HAL_I2C_Mem_Read(&hi2c3, FT5336_ADDRESS, FT5336_TD_STAT_REG,
                       I2C_MEMADD_SIZE_8BIT, registers, sizeof(registers),
                       20U) != HAL_OK ||
      (registers[0] & 0x0FU) == 0U)
  {
    data->state = LV_INDEV_STATE_RELEASED;
    touch_pressed = false;
    return;
  }

  uint16_t raw_x = (uint16_t)(((registers[1] & 0x0FU) << 8) | registers[2]);
  uint16_t raw_y = (uint16_t)(((registers[3] & 0x0FU) << 8) | registers[4]);

  /* The STM32F746G-DISCO panel is mounted with the FT5336 axes swapped. */
  data->point.x = raw_y < LCD_WIDTH ? raw_y : LCD_WIDTH - 1;
  data->point.y = raw_x < LCD_HEIGHT ? raw_x : LCD_HEIGHT - 1;
  data->state = LV_INDEV_STATE_PRESSED;
  if (!touch_pressed)
  {
    printf("[touch] press x=%ld y=%ld\r\n",
           (long)data->point.x, (long)data->point.y);
    touch_pressed = true;
  }
}

bool touch_ft5336_init(void)
{
  uint8_t chip_id = 0;
  if (HAL_I2C_Mem_Read(&hi2c3, FT5336_ADDRESS, FT5336_CHIP_ID_REG,
                       I2C_MEMADD_SIZE_8BIT, &chip_id, 1U, 100U) != HAL_OK ||
      chip_id != FT5336_CHIP_ID)
  {
    printf("[touch] FT5336 not found (id=0x%02X)\r\n", chip_id);
    return false;
  }

  lv_indev_t *input = lv_indev_create();
  lv_indev_set_type(input, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(input, touch_read);
  touch_ready = true;
  printf("[touch] FT5336 ready on I2C3\r\n");
  return true;
}
