#pragma once
#include <cstdint>

// Host stand-in for esp_timer_get_time(), microseconds since boot. Tests move
// fake_now_us to drive the location.phone_cell age limit (#140). It models the
// monotonic clock only, not timers, callbacks or wrap.
inline std::int64_t fake_now_us = 1000000;
inline std::int64_t esp_timer_get_time() { return fake_now_us; }
