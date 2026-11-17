#pragma once

namespace telemetry {
namespace contract {
// SHA-256 of telemetry_fields.inc; build and host tests verify exact bytes.
constexpr const char *kDictionarySha256 =
    "e7279c4edbe87071e4a51889124a43d7ffcd1977c79c8575887ae35bdee10903";
} // namespace contract
} // namespace telemetry
