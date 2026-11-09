#include "os_gnss.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <numeric>

namespace opensensor::gnss {
namespace {
constexpr std::string_view kInfoPrefix = "+CGNSSINFO:";
constexpr std::string_view kPowerPrefix = "+CGNSSPWR:";
// The manual fix reply has 18 fields. Room for a firmware that adds a few.
constexpr std::size_t kMaxFields = 24;
constexpr std::size_t kMaxFractionDigits = 10;

constexpr bool is_digit(char c) { return c >= '0' && c <= '9'; }

constexpr std::string_view trim(std::string_view text) {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\r' ||
                           text.front() == '\n' || text.front() == '\t'))
    text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\r' ||
                           text.back() == '\n' || text.back() == '\t'))
    text.remove_suffix(1);
  return text;
}

constexpr bool all_digits(std::string_view text) {
  return !text.empty() && std::all_of(text.begin(), text.end(), is_digit);
}

// The value of a digit run already checked by all_digits().
constexpr std::int64_t digits_value(std::string_view digits) {
  return std::accumulate(
      digits.begin(), digits.end(), std::int64_t{0},
      [](std::int64_t value, char c) { return value * 10 + (c - '0'); });
}

// The shape of a coordinate, digits, a dot and digits, before any check.
constexpr bool coordinate_shaped(std::string_view text) {
  const auto dot = text.find('.');
  return dot != std::string_view::npos && all_digits(text.substr(0, dot)) &&
         all_digits(text.substr(dot + 1));
}

// One axis of a coordinate. Latitude has at most 2 degree digits, longitude 3.
struct Axis {
  std::size_t degree_digits;
  int max_degrees;
};
constexpr Axis kLatitude{.degree_digits = 2, .max_degrees = 90};
constexpr Axis kLongitude{.degree_digits = 3, .max_degrees = 180};

// The manual V1.01 is self contradictory. Its Defined Values give plain
// decimal degrees (dd.ddddd, ddd.ddddd) and its example line, copied from
// SIM7600, gives degrees and minutes (ddmm.mmmm, dddmm.mmmm). Neither is
// confirmed on a V1.9.05 bench, so the integer digit count decides and
// anything else fails closed. Latitude with 1 or 2 integer digits and
// longitude with 1 to 3 are decimal degrees. Latitude with exactly 4 and
// longitude with exactly 5 are degrees * 100 + minutes, minutes below 60.
// Any other count is Malformed. Parsed by hand, no locale and no strtod.
std::expected<double, ParseError> coordinate(std::string_view text,
                                             const Axis &axis) {
  const auto dot = text.find('.');
  if (dot == std::string_view::npos)
    return std::unexpected(ParseError::Malformed);
  const auto whole = text.substr(0, dot);
  const auto fraction = text.substr(dot + 1);
  if (!all_digits(whole) || !all_digits(fraction) ||
      fraction.size() > kMaxFractionDigits)
    return std::unexpected(ParseError::Malformed);
  const bool decimal = whole.size() <= axis.degree_digits;
  if (!decimal && whole.size() != axis.degree_digits + 2)
    return std::unexpected(ParseError::Malformed);
  const auto integer = digits_value(whole);
  std::int64_t denominator = 1;
  for (std::size_t i = 0; i < fraction.size(); ++i)
    denominator *= 10;
  const double part = static_cast<double>(digits_value(fraction)) /
                      static_cast<double>(denominator);
  const auto degrees = decimal ? integer : integer / 100;
  const double minutes =
      decimal ? 0.0 : static_cast<double>(integer % 100) + part;
  const double rest = decimal ? part : minutes;
  if (minutes >= 60 || degrees > axis.max_degrees ||
      (degrees == axis.max_degrees && rest > 0))
    return std::unexpected(ParseError::OutOfRange);
  return decimal ? static_cast<double>(degrees) + part
                 : static_cast<double>(degrees) + minutes / 60;
}

constexpr int hemisphere(std::string_view text, char positive, char negative) {
  if (text.size() != 1)
    return 0;
  return text[0] == positive ? 1 : text[0] == negative ? -1 : 0;
}

// Satellites in view, the sum of the per system visible counts before the
// latitude (the manual's used count, NoSV, is the last field). -1 unknown.
std::int16_t satellites(const std::array<std::string_view, kMaxFields> &fields,
                        std::size_t latitude_index) {
  if (latitude_index != 5)
    return -1;
  int total = 0;
  bool any = false;
  for (std::size_t i = 1; i < 5; ++i) {
    if (fields[i].empty())
      continue;
    if (!all_digits(fields[i]) || fields[i].size() > 3)
      return -1;
    any = true;
    total += static_cast<int>(digits_value(fields[i]));
  }
  return any ? static_cast<std::int16_t>(total) : std::int16_t{-1};
}
} // namespace

std::expected<bool, ParseError>
SimcomDialect::parse_power(std::string_view line) const {
  line = trim(line);
  if (!line.starts_with(kPowerPrefix))
    return std::unexpected(ParseError::NotThisLine);
  const auto value = trim(line.substr(kPowerPrefix.size()));
  if (value == "1" || value == "READY!")
    return true;
  if (value == "0")
    return false;
  return std::unexpected(ParseError::Malformed);
}

std::expected<Reading, ParseError>
SimcomDialect::parse_fix(std::string_view line) const {
  line = trim(line);
  if (!line.starts_with(kInfoPrefix))
    return std::unexpected(ParseError::NotThisLine);
  const auto body = trim(line.substr(kInfoPrefix.size()));
  std::array<std::string_view, kMaxFields> fields{};
  std::size_t count = 0;
  bool any = false;
  for (std::size_t start = 0;;) {
    if (count == kMaxFields)
      return std::unexpected(ParseError::Malformed);
    const auto comma = body.find(',', start);
    const auto field = trim(body.substr(
        start, comma == std::string_view::npos ? comma : comma - start));
    any = any || !field.empty();
    fields[count++] = field;
    if (comma == std::string_view::npos)
      break;
    start = comma + 1;
  }
  // V1.9.05 answers ",,,,,,,," and the manual 18 empty fields.
  if (!any)
    return std::unexpected(ParseError::NoFix);
  std::size_t at = count;
  for (std::size_t i = 0; i + 3 < count; ++i)
    if (coordinate_shaped(fields[i]) && hemisphere(fields[i + 1], 'N', 'S') &&
        coordinate_shaped(fields[i + 2]) &&
        hemisphere(fields[i + 3], 'E', 'W')) {
      at = i;
      break;
    }
  if (at == count)
    return std::unexpected(ParseError::Malformed);
  Reading reading{};
  if (at > 0) {
    // A leading field, when present, is the fix mode, 2 for 2D or 3 for 3D.
    if (fields[0] != "2" && fields[0] != "3")
      return std::unexpected(ParseError::Malformed);
    reading.mode = static_cast<std::uint8_t>(fields[0][0] - '0');
  }
  const auto latitude = coordinate(fields[at], kLatitude);
  if (!latitude)
    return std::unexpected(latitude.error());
  const auto longitude = coordinate(fields[at + 2], kLongitude);
  if (!longitude)
    return std::unexpected(longitude.error());
  // Some receivers print zeros before the first fix. Null Island is no fix.
  if (*latitude == 0 && *longitude == 0)
    return std::unexpected(ParseError::NoFix);
  reading.latitude = *latitude * hemisphere(fields[at + 1], 'N', 'S');
  reading.longitude = *longitude * hemisphere(fields[at + 3], 'E', 'W');
  reading.satellites = satellites(fields, at);
  return reading;
}

const Dialect &simcom_dialect() {
  static const SimcomDialect dialect;
  return dialect;
}

std::expected<Fix, ParseError> to_fix(const Reading &reading,
                                      std::int64_t fix_mono_us) {
  location::Cell cell = 0;
  if (!location::from_degrees(reading.latitude, reading.longitude,
                              location::kPublishedResolution, cell))
    return std::unexpected(ParseError::OutOfRange);
  return Fix{.cell = cell,
             .resolution =
                 static_cast<std::uint8_t>(location::resolution(cell)),
             .fix_mono_us = fix_mono_us,
             .mode = reading.mode,
             .satellites = reading.satellites};
}

bool fresh(const Fix &fix, std::int64_t now_us) {
  if (fix.cell == 0 || fix.fix_mono_us <= 0)
    return false;
  const auto age = now_us - fix.fix_mono_us;
  return age <= kFixMaxAgeUs && age >= -kFixClockSkewUs;
}

std::expected<location::Cell, ReadbackError>
readback_cell(const Fix &fix, std::int64_t now_us) {
  if (fix.cell == 0)
    return std::unexpected(ReadbackError::NoFix);
  if (!fresh(fix, now_us))
    return std::unexpected(ReadbackError::Stale);
  const auto cell = location::published_cell(fix.cell);
  if (cell)
    return *cell;
  return std::unexpected(cell.error() == location::PublishError::TooCoarse
                             ? ReadbackError::TooCoarse
                             : ReadbackError::Invalid);
}

Selection select_location(const config::LocationSettings &row,
                          const std::optional<Fix> &fix, std::int64_t now_us) {
  // A phone cell wins for fixed and moving Things while it is set (#140).
  // apply_lines stores it at res 8, anything else is treated as unset.
  if (row.phone_cell) {
    const auto published = location::published_cell(row.phone_cell);
    if (published && *published == row.phone_cell)
      return {.cell = *published, .source = telemetry::LocationSource::Phone};
  }
  if (row.mobility == config::Mobility::Fixed) {
    const auto published = location::published_cell(row.cell);
    if (published && *published == row.cell)
      return {.cell = *published,
              .source = telemetry::LocationSource::Configured};
    return {};
  }
  if (!fix || !fresh(*fix, now_us))
    return {};
  if (const auto published = location::published_cell(fix->cell))
    return {.cell = *published, .source = telemetry::LocationSource::Gnss};
  return {};
}
} // namespace opensensor::gnss
