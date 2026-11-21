#include "os_logger_roots.h"

#include <array>
#include <cassert>

namespace {
using namespace opensensor::logger::roots;

void within() {
  assert(path_within("/sd/output", "/sd/output"));
  assert(path_within("/sd/output/thing=a", "/sd/output"));
  assert(!path_within("/sd/outputs", "/sd/output"));
  assert(!path_within("/sd", "/sd/output"));
  assert(path_within("/sd/output", "/sd"));
  // A missing root holds nothing.
  assert(!path_within("/sd/output", nullptr));
  assert(!path_within(nullptr, "/sd"));
}

constexpr Roots kBoard{.output = "/sd/output",
                       .benchmark = "/sd/bench",
                       .legacy = "/sd/parquet",
                       .fallback = "/flash/output"};

void board_roots_are_valid() {
  assert(fallback_valid(kBoard));
  Roots unset = kBoard;
  unset.fallback = nullptr;
  assert(fallback_valid(unset));
  Roots no_legacy = kBoard;
  no_legacy.legacy = nullptr;
  assert(fallback_valid(no_legacy));
}

void shape() {
  Roots roots = kBoard;
  roots.fallback = "flash/output"; // not absolute
  assert(!fallback_valid(roots));
  roots.fallback = "";
  assert(!fallback_valid(roots));
  roots.fallback = "/flash/0123456789012345678901234"; // 32 bytes
  assert(!fallback_valid(roots));
  roots.fallback = "/flash/012345678901234567890123"; // 31 bytes
  assert(fallback_valid(roots));
}

// The fallback root never holds, nor sits below, another root, so no walk
// of one root ever lists or moves a file of another.
void overlap() {
  constexpr std::array kBad{
      "/sd/output",  "/sd/output/flash", "/sd",           "/sd/bench",
      "/sd/bench/x", "/sd/parquet",      "/sd/parquet/f", "/"};
  for (const char *bad : kBad) {
    Roots roots = kBoard;
    roots.fallback = bad;
    assert(!fallback_valid(roots));
  }
  // A sibling with a shared prefix is fine.
  Roots roots = kBoard;
  roots.fallback = "/sd/outputs";
  assert(fallback_valid(roots));
  roots.fallback = "/sd/parquet2";
  assert(fallback_valid(roots));
}

} // namespace

int main() {
  within();
  board_roots_are_valid();
  shape();
  overlap();
  return 0;
}
