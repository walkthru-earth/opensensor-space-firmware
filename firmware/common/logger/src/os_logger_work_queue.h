#pragma once

#include "os_logger_watchdog.h"

#include <cstdint>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

namespace oslogger::work {
enum class StallAction : std::uint8_t { None, Warn };

// Copy first, then signal. The binary wakeup may coalesce several enqueues;
// the queues retain every item and the worker checks them before sleeping.
inline bool enqueue(QueueHandle_t queue, const void *item,
                    SemaphoreHandle_t wakeup) {
  if (xQueueSend(queue, item, 0) != pdTRUE)
    return false;
  xSemaphoreGive(wakeup);
  return true;
}

inline void wait_if_idle(QueueHandle_t samples, SemaphoreHandle_t wakeup) {
  if (uxQueueMessagesWaiting(samples) == 0)
    // An enqueue between the queue check and this take leaves a pending signal.
    // Periodic wakeup also keeps worker-heartbeat monitoring bounded.
    xSemaphoreTake(wakeup, pdMS_TO_TICKS(1000));
}

// The expiry rule is shared with the watchdog supervisor.
using watchdog::stall_expired;

// Evaluates storage-worker heartbeat liveness.
// - Returns None before the first heartbeat or within the 5 s normal window.
// - Returns Warn when elapsed > 5 s and >= 30 s have passed since the previous
//   warning.
inline StallAction check_stall(std::int64_t now_us, std::int64_t heartbeat_us,
                               std::int64_t &last_warning_us) {
  if (heartbeat_us <= 0 || now_us <= heartbeat_us)
    return StallAction::None;
  if (now_us - heartbeat_us > 5000000LL &&
      (last_warning_us == 0 || now_us - last_warning_us > 30000000LL)) {
    last_warning_us = now_us;
    return StallAction::Warn;
  }
  return StallAction::None;
}
} // namespace oslogger::work
