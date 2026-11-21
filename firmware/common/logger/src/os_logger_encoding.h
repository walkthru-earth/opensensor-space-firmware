#pragma once

// Value encoding of the INT32 and INT64 columns in every file the logger
// writes (#135). A board picks it at build time with the Kconfig switch
// CONFIG_OS_LOGGER_DELTA_INTEGERS, which defaults to n, so every board writes
// PLAIN until decision record 0002 records the board measurement (#61).
// FLOAT columns, the GEOMETRY column and the RLE definition levels never
// change. Pure and allocation free, so the host test compiles this header
// with the switch off and on and writes a file through the real writer.

#if __has_include(<sdkconfig.h>)
#include <sdkconfig.h>
#endif

#include "parquet_writer.h"

#include <utility>

namespace oslogger::encoding {

[[nodiscard]] constexpr telemetry::IntegerEncoding integers(bool delta) {
  return delta ? telemetry::IntegerEncoding::DeltaBinaryPacked
               : telemetry::IntegerEncoding::Plain;
}

// The parquet.thrift Encoding name, as the PARQUET BEGIN line prints it.
[[nodiscard]] constexpr const char *name(telemetry::IntegerEncoding encoding) {
  using enum telemetry::IntegerEncoding;
  switch (encoding) {
  case Plain:
    return "PLAIN";
  case DeltaBinaryPacked:
    return "DELTA_BINARY_PACKED";
  }
  return "unknown";
}

#if defined(CONFIG_OS_LOGGER_DELTA_INTEGERS) && CONFIG_OS_LOGGER_DELTA_INTEGERS
inline constexpr bool kDeltaIntegers = true;
#else
inline constexpr bool kDeltaIntegers = false;
#endif

// What os_logger.cpp passes to Writer::begin for every file it creates.
inline constexpr telemetry::IntegerEncoding kIntegers =
    integers(kDeltaIntegers);

static_assert(std::to_underlying(integers(false)) == 0,
              "PLAIN is parquet.thrift Encoding 0");
static_assert(std::to_underlying(integers(true)) == 5,
              "DELTA_BINARY_PACKED is parquet.thrift Encoding 5");

} // namespace oslogger::encoding
