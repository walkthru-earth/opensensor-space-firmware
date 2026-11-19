#pragma once

namespace telemetry {
namespace contract {
// SHA-256 of telemetry_fields.inc, checked by build.sh and host contract tests.
// It hashes raw bytes, so any edit to that file, comments and whitespace
// included, moves the digest and needs a new LIVE fixture and a phone release.
constexpr const char *kDictionarySha256 =
    "f4f81218dc80d1a8e6db043197a2f7180c4f075f9e8348df30eaceef44d68370";
} // namespace contract
} // namespace telemetry
