#include "app/stock_data.h"

#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

static stock_snapshot_t snapshots[APP_MAX_SYMBOLS];
static size_t snapshot_count;

void stock_data_publish(const stock_snapshot_t *snapshot)
{
  taskENTER_CRITICAL();
  size_t index;
  for (index = 0; index < snapshot_count; ++index)
  {
    if (strcmp(snapshots[index].symbol, snapshot->symbol) == 0)
    {
      break;
    }
  }
  if (index == snapshot_count && snapshot_count < APP_MAX_SYMBOLS)
  {
    ++snapshot_count;
  }
  if (index < APP_MAX_SYMBOLS)
  {
    snapshots[index] = *snapshot;
  }
  taskEXIT_CRITICAL();
}

bool stock_data_get(stock_snapshot_t *snapshot)
{
  bool available;

  taskENTER_CRITICAL();
  available = snapshot_count > 0U;
  if (available)
  {
    *snapshot = snapshots[snapshot_count - 1U];
  }
  taskEXIT_CRITICAL();

  return available;
}

size_t stock_data_get_all(stock_snapshot_t output[APP_MAX_SYMBOLS])
{
  taskENTER_CRITICAL();
  size_t count = snapshot_count;
  memcpy(output, snapshots, sizeof(snapshots));
  taskEXIT_CRITICAL();
  return count;
}
