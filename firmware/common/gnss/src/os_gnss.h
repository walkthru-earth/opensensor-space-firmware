#pragma once

#include <device_config.h>
#include <os_location.h>
#include <telemetry_logical.h>

#include <cstdint>
#include <expected>
#include <optional>
#include <string_view>

// Board-neutral GNSS support. Pure code, tested on the host. A receiver
// dialect turns one text line into a Reading, to_fix() turns that Reading
// into an H3 cell at once, and the raw position is dropped. Only the cell,
// its resolution and the fix monotonic time leave this header. Never log,
// persist or send a Reading.
namespace opensensor::gnss {

// Why a line gave no position. Anything other than a parsed fix is no fix.
enum class [[nodiscard]] ParseError : std::uint8_t {
  NotThisLine, // a different reply, an echo, OK or an URC
  NoFix,       // the receiver answered and has no position
  Malformed,   // the expected reply with fields that do not parse
  OutOfRange,  // parsed, but outside latitude 90 or longitude 180
};

// Raw position, stack only, between parse_fix() and to_fix().
struct Reading {
  double latitude = 0;          // degrees, north positive
  double longitude = 0;         // degrees, east positive
  std::uint8_t mode = 0;        // 2 for 2D, 3 for 3D, 0 when the line has none
  std::int16_t satellites = -1; // per system visible counts summed, -1 unknown
};

// What the logger may keep. cell is already at most the owner resolution.
struct Fix {
  location::Cell cell = 0;
  std::uint8_t resolution = 0;
  std::int64_t fix_mono_us = 0; // esp_timer time the reply was read
  std::uint8_t mode = 0;
  std::int16_t satellites = -1;
};

// One receiver family. An AT dialect polls with commands. A streaming NMEA
// dialect returns empty commands and parses the sentences it is sent.
// Commands carry no line ending, the reader adds CR LF.
class Dialect {
public:
  virtual ~Dialect() = default;
  virtual std::string_view power_query() const = 0;
  virtual std::string_view power_on() const = 0;
  virtual std::string_view fix_query() const = 0;
  // true when the engine reports on, false when off, an error otherwise.
  [[nodiscard]] virtual std::expected<bool, ParseError>
  parse_power(std::string_view line) const = 0;
  [[nodiscard]] virtual std::expected<Reading, ParseError>
  parse_fix(std::string_view line) const = 0;
};

// SIMCom A7670 and SIM7670 AT dialect, AT+CGNSSPWR and AT+CGNSSINFO. The
// SIM767XX AT Command Manual V1.01 gives decimal degrees in its Defined
// Values and degrees and minutes in its example, so both are accepted and
// told apart by the integer digit count, failing closed otherwise. The fix
// layout is matched by its latitude, N or S, longitude, E or W run, so a
// firmware that adds or drops leading fields still parses. Only a no-fix
// reply is confirmed on the bench, both fixed forms are unverified.
class SimcomDialect final : public Dialect {
public:
  std::string_view power_query() const override { return "AT+CGNSSPWR?"; }
  std::string_view power_on() const override { return "AT+CGNSSPWR=1"; }
  std::string_view fix_query() const override { return "AT+CGNSSINFO"; }
  [[nodiscard]] std::expected<bool, ParseError>
  parse_power(std::string_view line) const override;
  [[nodiscard]] std::expected<Reading, ParseError>
  parse_fix(std::string_view line) const override;
};

// Shared instance, safe to use from any task.
const Dialect &simcom_dialect();

// A fix older than this never labels a row. The reader polls about every
// 12 s (10 s delay plus up to 2 s for the reply), so one silent poll already
// takes the fix past this age and rows about 15 to 22 s after the last fix
// get location nulls instead of a stale cell.
constexpr std::int64_t kFixMaxAgeUs = 15'000'000;
// A row timestamped just before the reply was stored still counts.
constexpr std::int64_t kFixClockSkewUs = 1'000'000;

// Convert at once to a cell at `resolution` (0..15). The Reading is not kept.
[[nodiscard]] std::expected<Fix, ParseError>
to_fix(const Reading &reading, int resolution, std::int64_t fix_mono_us);

[[nodiscard]] bool fresh(const Fix &fix, std::int64_t now_us);

// GET_CONFIG page 1 location.gnss_cell (#147) shows the setup phone the cell
// the receiver sees, always at this resolution whatever the owner cap is.
inline constexpr int kReadbackResolution = 8;
// Why the readback is empty. Every error prints "".
enum class [[nodiscard]] ReadbackError : std::uint8_t {
  NoFix,     // the reader holds no fix
  Stale,     // older than kFixMaxAgeUs, the same rule as moving rows
  TooCoarse, // the owner cap is below 8, a cell is never upsampled
  Invalid,   // not an H3 cell
};
// The fix as a res 8 cell, when it is fresh at `now_us`. Never a coordinate.
[[nodiscard]] std::expected<location::Cell, ReadbackError>
readback_cell(const Fix &fix, std::int64_t now_us);

// The cell and source one row gets. source None means four location nulls.
struct Selection {
  location::Cell cell = 0;
  telemetry::LocationSource source = telemetry::LocationSource::None;
};

// Fixed keeps the configured cell, source 1, whatever the receiver says.
// Moving takes a fresh fix coarsened to the row's resolution cap, source 2.
// Moving without a fresh fix gets nulls, never the configured cell or a stale
// fix.
[[nodiscard]] Selection select_location(const config::LocationSettings &row,
                                        const std::optional<Fix> &fix,
                                        std::int64_t now_us);
} // namespace opensensor::gnss
