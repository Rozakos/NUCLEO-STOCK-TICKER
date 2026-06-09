#include "app/display.h"
#include "app/config.h"

#include <stdio.h>
#include <stdint.h>

#include "main.h"

extern SDRAM_HandleTypeDef hsdram1;
extern LTDC_HandleTypeDef hltdc;

#define SDRAM_TIMEOUT              0xFFFFU
#define SDRAM_REFRESH_COUNT        0x0603U
#define SDRAM_MODE_BURST_LENGTH_1  0x0000U
#define SDRAM_MODE_BURST_SEQUENTIAL 0x0000U
#define SDRAM_MODE_CAS_LATENCY_3   0x0030U
#define SDRAM_MODE_STANDARD        0x0000U
#define SDRAM_MODE_WRITEBURST_SINGLE 0x0200U

#define RGB565_BLACK   0x0000U
static int sdram_send_command(uint32_t mode, uint32_t auto_refresh, uint32_t mode_register)
{
  FMC_SDRAM_CommandTypeDef command = {0};

  command.CommandMode = mode;
  command.CommandTarget = FMC_SDRAM_CMD_TARGET_BANK1;
  command.AutoRefreshNumber = auto_refresh;
  command.ModeRegisterDefinition = mode_register;

  return HAL_SDRAM_SendCommand(&hsdram1, &command, SDRAM_TIMEOUT) == HAL_OK ? 0 : -1;
}

static int sdram_start(void)
{
  const uint32_t mode_register =
      SDRAM_MODE_BURST_LENGTH_1 |
      SDRAM_MODE_BURST_SEQUENTIAL |
      SDRAM_MODE_CAS_LATENCY_3 |
      SDRAM_MODE_STANDARD |
      SDRAM_MODE_WRITEBURST_SINGLE;

  if (sdram_send_command(FMC_SDRAM_CMD_CLK_ENABLE, 1U, 0U) != 0)
  {
    return -1;
  }
  HAL_Delay(1);

  if (sdram_send_command(FMC_SDRAM_CMD_PALL, 1U, 0U) != 0 ||
      sdram_send_command(FMC_SDRAM_CMD_AUTOREFRESH_MODE, 8U, 0U) != 0 ||
      sdram_send_command(FMC_SDRAM_CMD_LOAD_MODE, 1U, mode_register) != 0 ||
      HAL_SDRAM_ProgramRefreshRate(&hsdram1, SDRAM_REFRESH_COUNT) != HAL_OK)
  {
    return -1;
  }

  return 0;
}

static int sdram_verify(void)
{
  volatile uint32_t *const test = (volatile uint32_t *)LCD_FB_BASE_ADDR;
  const uint32_t pattern[] = {0x00000000U, 0xFFFFFFFFU, 0xA5A55A5AU, 0x5A5AA5A5U};

  for (uint32_t i = 0; i < 4U; ++i)
  {
    test[i] = pattern[i];
  }

  for (uint32_t i = 0; i < 4U; ++i)
  {
    if (test[i] != pattern[i])
    {
      return -1;
    }
  }

  return 0;
}

static void clear_framebuffer(void)
{
  volatile uint16_t *const framebuffer = (volatile uint16_t *)LCD_FB_BASE_ADDR;

  for (uint32_t i = 0; i < LCD_WIDTH * LCD_HEIGHT; ++i)
  {
    framebuffer[i] = RGB565_BLACK;
  }
}

int display_init(void)
{
  if (sdram_start() != 0)
  {
    printf("[display] SDRAM init FAILED\r\n");
    return -1;
  }

  if (sdram_verify() != 0)
  {
    printf("[display] SDRAM verify FAILED\r\n");
    return -1;
  }

  clear_framebuffer();
  HAL_LTDC_Reload(&hltdc, LTDC_RELOAD_IMMEDIATE);
  HAL_GPIO_WritePin(LCD_DISP_GPIO_Port, LCD_DISP_Pin, GPIO_PIN_SET);
  HAL_GPIO_WritePin(LCD_BL_CTRL_GPIO_Port, LCD_BL_CTRL_Pin, GPIO_PIN_SET);
  printf("[display] SDRAM and RGB565 framebuffer ready\r\n");
  return 0;
}
