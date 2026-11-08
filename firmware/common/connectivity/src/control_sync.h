#pragma once

#include "ble_sync.h"

#include <span>

namespace ossync {
// Synchronous worker-side replies; may wrap the same BLE/LAN response adapters
// used by ArchiveSession. A reply can block on transport back-pressure, so this
// service must never execute on a radio callback task.
struct ControlReplies {
  void *context = nullptr;
  std::uint16_t (*payload_max)(void *, ble::Link) = nullptr;
  bool (*respond)(void *, ble::Link, const std::uint8_t *,
                  std::size_t) = nullptr;
  bool (*error)(void *, ble::Link, ble::Op, ble::Error, const char *) = nullptr;
  // The board template GET_DESCRIPTOR serves, without its NUL. It must live
  // in .rodata for the program's lifetime. Empty answers ERROR no-descriptor.
  std::span<const char> descriptor{};
  // GET_CONFIG page 1 location.gnss_cell (#147). Returns the newest fresh fix
  // as a res 8 cell, or 0 for none. Runs on the worker, so it must not block.
  // nullptr means the board has no receiver and the key is omitted.
  std::uint64_t (*gnss_cell)(void *) = nullptr;
};

// Executes GET/SET_CONFIG, LOG_TAIL, GET_DIAG, idle ABORT, GET_TOKEN,
// GET_DESCRIPTOR and busy WIFI_SCAN replies.
// Clock, STATUS, flush and reboot remain runtime operations. One command worker
// owns this service and config writes; returns false for a runtime operation.
bool handle_common_control(const ble::ControlRequest &request,
                           const ControlReplies &replies);
} // namespace ossync
