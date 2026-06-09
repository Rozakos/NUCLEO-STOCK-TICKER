#include "app/history_data.h"

#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

static history_request_t pending_request;
static history_snapshot_t current_snapshot;
static bool request_pending;
static uint32_t current_generation;

uint32_t history_data_request(const char *symbol, const char *range)
{
  taskENTER_CRITICAL();
  ++current_generation;
  memset(&pending_request, 0, sizeof(pending_request));
  strncpy(pending_request.symbol, symbol, sizeof(pending_request.symbol) - 1U);
  strncpy(pending_request.range, range, sizeof(pending_request.range) - 1U);
  pending_request.generation = current_generation;
  request_pending = true;
  current_snapshot.error = false;
  current_snapshot.fresh = false;
  uint32_t generation = current_generation;
  taskEXIT_CRITICAL();
  return generation;
}

bool history_data_request_pending(void)
{
  taskENTER_CRITICAL();
  bool pending = request_pending;
  taskEXIT_CRITICAL();
  return pending;
}

bool history_data_take_request(history_request_t *request)
{
  taskENTER_CRITICAL();
  bool pending = request_pending;
  if (pending)
  {
    *request = pending_request;
    request_pending = false;
  }
  taskEXIT_CRITICAL();
  return pending;
}

bool history_data_request_current(uint32_t generation)
{
  taskENTER_CRITICAL();
  bool current = generation == current_generation;
  taskEXIT_CRITICAL();
  return current;
}

void history_data_publish(const history_snapshot_t *snapshot)
{
  taskENTER_CRITICAL();
  if (snapshot->generation == current_generation)
  {
    current_snapshot = *snapshot;
  }
  taskEXIT_CRITICAL();
}

void history_data_publish_error(const history_request_t *request,
                                const char *status)
{
  history_snapshot_t snapshot = { 0 };
  strncpy(snapshot.symbol, request->symbol, sizeof(snapshot.symbol) - 1U);
  strncpy(snapshot.range, request->range, sizeof(snapshot.range) - 1U);
  strncpy(snapshot.status, status, sizeof(snapshot.status) - 1U);
  snapshot.generation = request->generation;
  snapshot.error = true;
  history_data_publish(&snapshot);
}

bool history_data_get(history_snapshot_t *snapshot)
{
  taskENTER_CRITICAL();
  bool available = current_snapshot.generation != 0U;
  if (available)
  {
    *snapshot = current_snapshot;
  }
  taskEXIT_CRITICAL();
  return available;
}
