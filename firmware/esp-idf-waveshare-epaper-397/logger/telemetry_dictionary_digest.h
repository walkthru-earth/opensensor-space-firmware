#pragma once

namespace telemetry {
namespace contract {
// SHA-256 of telemetry_fields.inc; build and host tests verify exact bytes.
constexpr const char *kDictionarySha256 =
    "a19c450b6fc5bc4bb76e9b52e03aa75bee05cd3c1505d961771279366c820439";
} // namespace contract
} // namespace telemetry
