// Host tests for firmware/common/gnss. Parser dialect, fix to cell,
// freshness, the per row source rule and that a GNSS cell never splits files
// while a deployment id change does.
#include <device_config.h>
#include <numeric_sample.h>
#include <nvs.h>
#include <os_gnss.h>
#include <os_logger_location.h>

#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

using namespace opensensor;
using gnss::ParseError;
using telemetry::LocationSource;

namespace {
const gnss::Dialect &simcom = gnss::simcom_dialect();

ParseError error_of(std::string_view line) {
  const auto reading = simcom.parse_fix(line);
  assert(!reading);
  return reading.error();
}

gnss::Reading reading_of(std::string_view line) {
  const auto reading = simcom.parse_fix(line);
  assert(reading);
  return *reading;
}

void parser() {
  // Bench, SIM7670G-MNGV V1.9.05 without a fix, 2026-10-27.
  assert(error_of("+CGNSSINFO: ,,,,,,,,") == ParseError::NoFix);
  assert(error_of("+CGNSSINFO: ,,,,,,,,\r") == ParseError::NoFix);
  // Manual V1.01 no fix form, 18 empty fields.
  assert(error_of("+CGNSSINFO: ,,,,,,,,,,,,,,,,,") == ParseError::NoFix);
  assert(error_of("+CGNSSINFO:") == ParseError::NoFix);

  // Manual V1.01 fix example. 31 deg 13.330650 min N, 121 deg 21.262554 min E.
  auto fix = reading_of("+CGNSSINFO: 2,09,05,00,00,3113.330650,N,12121.262554,"
                        "E,131117,091918.00,32.9,0.0,255.0,1.1,0.8,0.7,14");
  assert(fix.mode == 2 && fix.satellites == 14);
  assert(std::abs(fix.latitude - (31 + 13.330650 / 60)) < 1e-9);
  assert(std::abs(fix.longitude - (121 + 21.262554 / 60)) < 1e-9);
  // 3D fix, southern and western hemispheres, empty per system counts.
  fix = reading_of("+CGNSSINFO: 3,,,,,3001.500000,S,03114.250000,W,271026,"
                   "101500.0,10.0,0.0,0.0,1.0,1.0,1.0,7");
  assert(fix.mode == 3 && fix.satellites == -1);
  assert(std::abs(fix.latitude + (30 + 1.5 / 60)) < 1e-9);
  assert(std::abs(fix.longitude + (31 + 14.25 / 60)) < 1e-9);
  // Tolerant layout. The CGPSINFO style run with no mode field, and a layout
  // with only the mode before the latitude.
  fix = reading_of("+CGNSSINFO: 3113.343286,N,12121.234064,E,250311,072809.33,"
                   "44.1,0.0,0");
  assert(fix.mode == 0 && fix.satellites == -1);
  assert(std::abs(fix.latitude - (31 + 13.343286 / 60)) < 1e-9);
  fix = reading_of("+CGNSSINFO: 3,3002.0,N,03114.0,E");
  assert(fix.mode == 3 && std::abs(fix.longitude - (31 + 14.0 / 60)) < 1e-9);
  // Exactly on the limits.
  fix = reading_of("+CGNSSINFO: 2,,,,,9000.0,N,18000.0,W");
  assert(fix.latitude == 90 && fix.longitude == -180);

  // Manual V1.01 Defined Values form, plain decimal degrees (dd.ddddd and
  // ddd.ddddd). Cairo must stay Cairo, never be read as ddmm.
  fix = reading_of("+CGNSSINFO: 3,13,14,,,30.0444190,N,31.2357120,E,271026,"
                   "101500.0,10.0,0.0,0.0,1.0,1.0,1.0,9");
  assert(fix.mode == 3 && fix.satellites == 27);
  assert(std::abs(fix.latitude - 30.044419) < 1e-9);
  assert(std::abs(fix.longitude - 31.235712) < 1e-9);
  // Southern and western decimal, one degree digit each.
  fix = reading_of("+CGNSSINFO: 3,13,14,,,5.123456,S,7.654321,W");
  assert(std::abs(fix.latitude + 5.123456) < 1e-9);
  assert(std::abs(fix.longitude + 7.654321) < 1e-9);
  // Three degree digit decimal longitude and the decimal limits.
  fix = reading_of("+CGNSSINFO: 2,,,,,31.5,N,121.25,E");
  assert(fix.latitude == 31.5 && fix.longitude == 121.25);
  fix = reading_of("+CGNSSINFO: 2,,,,,90.0,S,180.0,E");
  assert(fix.latitude == -90 && fix.longitude == 180);

  // Not a CGNSSINFO reply.
  for (std::string_view line :
       {"", "OK", "ERROR", "AT+CGNSSINFO", "+CGNSSPWR: 1",
        "+CGPSINFO: ,,,,,,,,", "+CGNSSPWR: READY!",
        " CGNSSINFO: 2,,,,,3113.0,N,12121.0,E"})
    assert(error_of(line) == ParseError::NotThisLine);
  // Malformed, never a fix.
  for (std::string_view line : {
           "+CGNSSINFO: garbage",
           "+CGNSSINFO: 2,09,05,00,00,3113.330650,X,12121.262554,E",
           "+CGNSSINFO: 2,09,05,00,00,3113.330650,N,12121.262554,Q",
           "+CGNSSINFO: 2,09,05,00,00,3113.330650,N,,E",
           "+CGNSSINFO: 2,09,05,00,00,3113,N,12121.262554,E",
           "+CGNSSINFO: 2,09,05,00,00,31a3.330650,N,12121.262554,E",
           "+CGNSSINFO: 2,09,05,00,00,-3113.330650,N,12121.262554,E",
           "+CGNSSINFO: 2,09,05,00,00,3113.330650,NN,12121.262554,E",
           // A mode other than 2D or 3D in front of a position.
           "+CGNSSINFO: 1,09,05,00,00,3113.330650,N,12121.262554,E",
           "+CGNSSINFO: ,09,05,00,00,3113.330650,N,12121.262554,E",
           "+CGNSSINFO: x,3113.330650,N,12121.262554,E",
           // Too many integer digits, a ddmm with 5 or dddmm with 6.
           "+CGNSSINFO: 2,,,,,31130.0,N,12121.0,E",
           "+CGNSSINFO: 2,,,,,3113.0,N,121210.0,E",
           // Neither decimal degrees nor ddmm, fail closed. A 3 digit
           // latitude and a 4 digit longitude.
           "+CGNSSINFO: 2,,,,,311.5,N,12121.0,E",
           "+CGNSSINFO: 2,,,,,3113.0,N,3114.0,E",
           "+CGNSSINFO: 3,3002.0,N,3114.0,E",
           // Fraction longer than ten digits.
           "+CGNSSINFO: 2,,,,,3113.12345678901,N,12121.0,E",
           // More than 24 fields.
           "+CGNSSINFO: 2,,,,,3113.0,N,12121.0,E,,,,,,,,,,,,,,,,,,,,,,,,,,",
       })
    assert(error_of(line) == ParseError::Malformed);
  // Out of range, never a fix.
  for (std::string_view line : {
           "+CGNSSINFO: 2,,,,,9100.0,N,12121.0,E",
           "+CGNSSINFO: 2,,,,,9000.1,N,12121.0,E",
           "+CGNSSINFO: 2,,,,,3160.0,N,12121.0,E",
           "+CGNSSINFO: 2,,,,,3113.0,N,18100.0,E",
           "+CGNSSINFO: 2,,,,,3113.0,N,18000.5,E",
           "+CGNSSINFO: 2,,,,,3113.0,N,12199.0,E",
           // Minutes of 60 or more in ddmm, latitude and longitude.
           "+CGNSSINFO: 2,,,,,3060.000001,N,12121.0,E",
           "+CGNSSINFO: 2,,,,,3113.0,N,03175.5,E",
           // Decimal degrees past the limits.
           "+CGNSSINFO: 2,,,,,91.0,N,31.0,E",
           "+CGNSSINFO: 2,,,,,90.000001,N,31.0,E",
           "+CGNSSINFO: 2,,,,,30.0,N,181.0,E",
           "+CGNSSINFO: 2,,,,,30.0,N,180.5,W",
       })
    assert(error_of(line) == ParseError::OutOfRange);
  // Zeros before a first fix are no fix, not Null Island.
  assert(error_of("+CGNSSINFO: 2,,,,,0000.0,N,00000.0,E") == ParseError::NoFix);

  // Engine power replies.
  assert(simcom.parse_power("+CGNSSPWR: 1") == true);
  assert(simcom.parse_power("+CGNSSPWR: 0\r") == false);
  assert(simcom.parse_power("+CGNSSPWR: READY!") == true);
  assert(simcom.parse_power("+CGNSSPWR: 2").error() == ParseError::Malformed);
  assert(simcom.parse_power("OK").error() == ParseError::NotThisLine);
  assert(simcom.power_query() == "AT+CGNSSPWR?");
  assert(simcom.power_on() == "AT+CGNSSPWR=1");
  assert(simcom.fix_query() == "AT+CGNSSINFO");
}

void conversion() {
  // H3 quick start fixture, San Francisco, res 9 0x8928308280fffff.
  const gnss::Reading reading{.latitude = 37.775938728915946,
                              .longitude = -122.41795063018799,
                              .mode = 3,
                              .satellites = 9};
  // Converted straight to res 8, never finer (#139).
  const auto fix = gnss::to_fix(reading, 1'000'000);
  assert(fix && fix->cell == 0x8828308281fffffULL && fix->resolution == 8);
  assert(fix->fix_mono_us == 1'000'000 && fix->mode == 3 &&
         fix->satellites == 9);
  assert(gnss::to_fix({.latitude = 91}, 1).error() == ParseError::OutOfRange);
  assert(gnss::to_fix({.longitude = -181}, 1).error() ==
         ParseError::OutOfRange);
}

void freshness() {
  const gnss::Fix fix{.cell = 0x8828308281fffffULL,
                      .resolution = 8,
                      .fix_mono_us = 100'000'000};
  assert(gnss::fresh(fix, 100'000'000));
  assert(gnss::fresh(fix, 100'000'000 + gnss::kFixMaxAgeUs));
  assert(!gnss::fresh(fix, 100'000'001 + gnss::kFixMaxAgeUs));
  assert(gnss::fresh(fix, 100'000'000 - gnss::kFixClockSkewUs));
  assert(!gnss::fresh(fix, 99'999'999 - gnss::kFixClockSkewUs));
  auto empty = fix;
  empty.cell = 0;
  assert(!gnss::fresh(empty, 100'000'000));
  auto unset = fix;
  unset.fix_mono_us = 0;
  assert(!gnss::fresh(unset, 1));
}

// GET_CONFIG page 1 location.gnss_cell (#147). Always res 8 and empty
// without a fresh fix.
void readback() {
  using gnss::ReadbackError;
  static_assert(gnss::kReadbackResolution == 8);
  constexpr std::int64_t now = 100'000'000;
  constexpr location::Cell kAt8 = 0x8828308281fffffULL;
  const gnss::Fix fine{
      .cell = 0x8928308280fffffULL, .resolution = 9, .fix_mono_us = now};
  auto cell = gnss::readback_cell(fine, now);
  assert(cell && *cell == kAt8 && location::resolution(*cell) == 8);
  const gnss::Fix at8{.cell = kAt8, .resolution = 8, .fix_mono_us = now};
  cell = gnss::readback_cell(at8, now + gnss::kFixMaxAgeUs);
  assert(cell && *cell == kAt8);
  assert(gnss::readback_cell(at8, now + gnss::kFixMaxAgeUs + 1).error() ==
         ReadbackError::Stale);
  auto unset = at8;
  unset.fix_mono_us = 0;
  assert(gnss::readback_cell(unset, now).error() == ReadbackError::Stale);
  assert(gnss::readback_cell({}, now).error() == ReadbackError::NoFix);
  // A fix coarser than res 8 cannot be upsampled, so the readback is empty.
  const gnss::Fix coarse{
      .cell = 0x85283083fffffffULL, .resolution = 5, .fix_mono_us = now};
  assert(gnss::readback_cell(coarse, now).error() == ReadbackError::TooCoarse);
  const gnss::Fix broken{.cell = 1, .resolution = 8, .fix_mono_us = now};
  assert(gnss::readback_cell(broken, now).error() == ReadbackError::Invalid);
}

constexpr location::Cell kConfigured = 0x8828308281fffffULL; // res 8
constexpr location::Cell kCoarse = 0x85283083fffffffULL;     // res 5

// A res 15 cell in Cairo, far from the configured cell, with its res 9 and
// res 8 ancestors.
struct CairoCells {
  location::Cell at15 = 0, at9 = 0, at8 = 0;
};
CairoCells cairo() {
  CairoCells cells;
  assert(location::from_degrees(30.0444, 31.2357, 15, cells.at15));
  assert(location::coarsen(cells.at15, 9, cells.at9));
  assert(location::coarsen(cells.at15, 8, cells.at8));
  assert(location::resolution(cells.at8) == 8 && cells.at8 != kConfigured);
  return cells;
}

void source_rule() {
  const std::int64_t now = 50'000'000;
  const auto cells = cairo();
  const gnss::Fix fresh_fix{
      .cell = cells.at9, .resolution = 9, .fix_mono_us = now - 1'000'000};
  const gnss::Fix stale_fix{.cell = cells.at9,
                            .resolution = 9,
                            .fix_mono_us = now - gnss::kFixMaxAgeUs - 1};
  config::LocationSettings fixed{.cell = kConfigured,
                                 .resolution = 8,
                                 .country = "US",
                                 .mobility = config::Mobility::Fixed};
  // Fixed keeps the configured cell, with or without a fix.
  for (const auto &fix : {std::optional<gnss::Fix>{}, std::optional{fresh_fix},
                          std::optional{stale_fix}}) {
    const auto chosen = gnss::select_location(fixed, fix, now);
    assert(chosen.cell == kConfigured);
    assert(chosen.source == LocationSource::Configured);
  }
  // A fixed snapshot that is not res 8, finer or coarser, or unset, gives
  // nulls (#139).
  for (const location::Cell other : {cells.at9, kCoarse}) {
    auto off = fixed;
    off.cell = other;
    assert(gnss::select_location(off, fresh_fix, now).source ==
           LocationSource::None);
  }
  assert(gnss::select_location({}, fresh_fix, now).source ==
         LocationSource::None);

  auto moving = fixed;
  moving.mobility = config::Mobility::Moving;
  // Moving with a fresh fix publishes its res 8 parent, source 2.
  auto chosen = gnss::select_location(moving, fresh_fix, now);
  assert(chosen.cell == cells.at8 && chosen.source == LocationSource::Gnss);
  auto finest = fresh_fix;
  finest.cell = cells.at15;
  chosen = gnss::select_location(moving, finest, now);
  assert(chosen.cell == cells.at8 && chosen.source == LocationSource::Gnss);
  // A fix coarser than res 8 is never upsampled and gives nulls.
  auto coarse_fix = fresh_fix;
  coarse_fix.cell = kCoarse;
  chosen = gnss::select_location(moving, coarse_fix, now);
  assert(chosen.cell == 0 && chosen.source == LocationSource::None);
  // Moving with a stale fix or none writes nulls, never the configured cell.
  for (const auto &fix :
       {std::optional<gnss::Fix>{}, std::optional{stale_fix}}) {
    chosen = gnss::select_location(moving, fix, now);
    assert(chosen.cell == 0 && chosen.source == LocationSource::None);
  }
  // Moving with an unconfigured cell still takes a fresh fix.
  auto unconfigured = moving;
  unconfigured.cell = 0;
  chosen = gnss::select_location(unconfigured, fresh_fix, now);
  assert(chosen.cell == cells.at8 && chosen.source == LocationSource::Gnss);
}

void row_mapping() {
  oslogger::LocationSample<NumericSample<4>> row{};
  // Source 2 lands in location_source with the cell centre as the point.
  const auto fix_at8 = cairo().at8;
  assert(
      oslogger::apply_cell(row, fix_at8, LocationSource::Gnss, {0, 1, 2, 3}));
  assert(row.data[0] == static_cast<std::int64_t>(fix_at8) && row.data[1] == 8);
  assert(row.data[2] == 2 && row.valid[0] && row.valid[1] && row.valid[2] &&
         row.valid[3]);
  std::int64_t lat = 0, lon = 0;
  assert(location::center_e7(fix_at8, lat, lon));
  assert(row.data[3] ==
         telemetry::pack_point_e7(static_cast<std::int32_t>(lon),
                                  static_cast<std::int32_t>(lat)));
  // None clears every location field.
  assert(
      !oslogger::apply_cell(row, fix_at8, LocationSource::None, {0, 1, 2, 3}));
  for (const auto present : row.valid)
    assert(!present);
  assert(!oslogger::apply_cell(row, 0, LocationSource::Gnss, {0, 1, 2, 3}));
  for (const auto present : row.valid)
    assert(!present);
}

// The logger splits a file when same_location() is false between the open
// batch and a new row. A GNSS cell lives only in the row columns, so two
// moving rows with different fixes share one snapshot and one file. A
// mobility change still splits.
void no_split() {
  const std::int64_t now = 50'000'000;
  config::LocationSettings moving{.cell = kConfigured,
                                  .resolution = 8,
                                  .country = "EG",
                                  .mobility = config::Mobility::Moving};
  oslogger::LocationSample<NumericSample<4>> first{}, second{};
  first.location = second.location = moving;
  const gnss::Fix here{
      .cell = 0x8928308280fffffULL, .resolution = 9, .fix_mono_us = now};
  const gnss::Fix there{
      .cell = cairo().at9, .resolution = 9, .fix_mono_us = now};
  const auto a = gnss::select_location(first.location, here, now);
  const auto b = gnss::select_location(second.location, there, now);
  assert(oslogger::apply_cell(first, a.cell, a.source, {0, 1, 2, 3}));
  assert(oslogger::apply_cell(second, b.cell, b.source, {0, 1, 2, 3}));
  assert(first.data[0] != second.data[0]);
  assert(config::same_location(first.location, second.location));
  auto parked = second.location;
  parked.mobility = config::Mobility::Fixed;
  assert(!config::same_location(first.location, parked));
}

// The deployment id is a file label like the location (#148). The logger's
// fits_batch() compares labels with same_file_labels(), so a deployment id
// change closes the file and an owner title change never does.
void deployment_split() {
  using Sample = oslogger::LocationSample<NumericSample<4>>;
  fake_platform::nvs = {};
  assert(config::load(false));
  const auto set = [](std::string_view lines) {
    config::Actions actions{};
    char bad_key[32]{};
    assert(config::apply_lines(lines.data(), lines.size(), bad_key,
                               sizeof bad_key, actions));
  };
  const auto snapshot = [] {
    Sample row{};
    const auto labels = ::config::sample_labels();
    row.location = labels.location;
    row.deployment = labels.deployment;
    return row;
  };
  const auto first = snapshot();
  assert(!first.deployment.text[0]);
  set("owner.title=Cairo plots\nowner.provider=Walkthru\n"
      "owner.url=https://walkthru.earth\nowner.description=Roof");
  assert(oslogger::same_file_labels(snapshot(), first));
  set("thing.deployment_id=cairo-07");
  const auto second = snapshot();
  assert(std::string_view(second.deployment.text.data()) == "cairo-07");
  assert(!oslogger::same_file_labels(second, first));
  assert(!oslogger::same_file_labels(first, second));
  // The earlier snapshot keeps its value, rows are never relabelled.
  assert(!first.deployment.text[0]);
  set("thing.deployment_id=cairo-07\nowner.title=Other");
  assert(oslogger::same_file_labels(snapshot(), second));
  set("thing.deployment_id=");
  assert(oslogger::same_file_labels(snapshot(), first));
  // A location change still splits with the same deployment id.
  auto moved = first;
  moved.location.mobility = config::Mobility::Moving;
  assert(!oslogger::same_file_labels(moved, first));
}

// location.phone_cell (#140). While set it labels every row with source 3,
// fixed or moving, ahead of the configured cell and a fresh GNSS fix. A fixed
// Thing splits its file when the effective cell changes. A moving Thing never
// splits on a phone cell change. Clearing it or a reboot falls back.
void phone_cell_rule() {
  using Sample = oslogger::LocationSample<NumericSample<4>>;
  const std::int64_t now = 50'000'000;
  const auto cells = cairo();
  const gnss::Fix fresh_fix{
      .cell = cells.at9, .resolution = 9, .fix_mono_us = now - 1'000'000};
  fake_platform::nvs = {};
  assert(config::load(false));
  const auto set = [](std::string_view lines) {
    config::Actions actions{};
    char bad_key[32]{};
    assert(config::apply_lines(lines.data(), lines.size(), bad_key,
                               sizeof bad_key, actions));
  };
  const auto snapshot = [] {
    Sample row{};
    const auto labels = ::config::sample_labels();
    row.location = labels.location;
    row.deployment = labels.deployment;
    return row;
  };
  const auto chosen = [&](const Sample &row) {
    return gnss::select_location(row.location, fresh_fix, now);
  };
  // A res 8 phone cell (Cairo), and another res 8 cell next to it.
  location::Cell neighbour = 0;
  assert(location::from_degrees(30.0644, 31.2357, 8, neighbour));
  assert(neighbour != cells.at8 && location::resolution(neighbour) == 8);
  char phone_text[location::kCellTextBytes]{};
  char fine_text[location::kCellTextBytes]{};
  char neighbour_text[location::kCellTextBytes]{};
  assert(location::format_cell(cells.at8, phone_text, sizeof phone_text));
  assert(location::format_cell(cells.at15, fine_text, sizeof fine_text));
  assert(
      location::format_cell(neighbour, neighbour_text, sizeof neighbour_text));
  const auto phone_line = [](const char *text) {
    return std::string("location.phone_cell=") + text;
  };

  // Fixed Thing with a configured cell, source 1 before the phone cell.
  set("location.cell=8828308281fffff");
  const auto configured = snapshot();
  auto pick = chosen(configured);
  assert(pick.cell == kConfigured && pick.source == LocationSource::Configured);
  // A finer phone cell is kept as its res 8 parent and wins, source 3.
  set(phone_line(fine_text));
  const auto at_phone = snapshot();
  assert(at_phone.location.phone_cell == cells.at8);
  assert(at_phone.location.cell == kConfigured);
  pick = chosen(at_phone);
  assert(pick.cell == cells.at8 && pick.source == LocationSource::Phone);
  Sample row = at_phone;
  assert(oslogger::apply_cell(row, pick.cell, pick.source, {0, 1, 2, 3}));
  assert(row.data[0] == static_cast<std::int64_t>(cells.at8) &&
         row.data[1] == 8 && row.data[2] == 3);
  // The effective cell moved, so a fixed Thing closes the file.
  assert(!oslogger::same_file_labels(at_phone, configured));
  assert(!oslogger::same_file_labels(configured, at_phone));
  // The same phone cell again keeps the file.
  set(phone_line(phone_text));
  assert(oslogger::same_file_labels(snapshot(), at_phone));
  // An update to another cell splits a fixed Thing again.
  set(phone_line(neighbour_text));
  const auto updated = snapshot();
  assert(chosen(updated).cell == neighbour);
  assert(!oslogger::same_file_labels(updated, at_phone));
  // A phone cell equal to the configured cell keeps the effective cell.
  set("location.phone_cell=8828308281fffff");
  const auto same_cell = snapshot();
  assert(chosen(same_cell).source == LocationSource::Phone);
  assert(oslogger::same_file_labels(same_cell, configured));
  // Clearing falls back to the configured cell, source 1, and splits back.
  set("location.phone_cell=");
  const auto cleared = snapshot();
  assert(cleared.location.phone_cell == 0);
  pick = chosen(cleared);
  assert(pick.cell == kConfigured && pick.source == LocationSource::Configured);
  assert(!oslogger::same_file_labels(cleared, updated));
  assert(oslogger::same_file_labels(cleared, configured));
  // A fixed Thing with no configured cell still takes the phone cell.
  set("location.cell=\n" + phone_line(phone_text));
  pick = chosen(snapshot());
  assert(pick.cell == cells.at8 && pick.source == LocationSource::Phone);

  // Moving Thing. The phone cell beats a fresh fix, and a change never splits.
  set("thing.mobility=moving\n" + phone_line(phone_text));
  const auto moving_a = snapshot();
  pick = chosen(moving_a);
  assert(pick.cell == cells.at8 && pick.source == LocationSource::Phone);
  set(phone_line(neighbour_text));
  const auto moving_b = snapshot();
  pick = chosen(moving_b);
  assert(pick.cell == neighbour && pick.source == LocationSource::Phone);
  assert(oslogger::same_file_labels(moving_a, moving_b));
  assert(oslogger::same_file_labels(moving_b, moving_a));
  // Cleared, a moving Thing goes back to a fresh fix or four nulls, still in
  // the same file.
  set("location.phone_cell=");
  const auto moving_none = snapshot();
  pick = chosen(moving_none);
  assert(pick.cell == cells.at8 && pick.source == LocationSource::Gnss);
  pick = gnss::select_location(moving_none.location, std::nullopt, now);
  assert(pick.cell == 0 && pick.source == LocationSource::None);
  assert(oslogger::same_file_labels(moving_none, moving_b));
  // Without a fix the phone cell still labels a moving row.
  pick = gnss::select_location(moving_b.location, std::nullopt, now);
  assert(pick.cell == neighbour && pick.source == LocationSource::Phone);
  // A mobility change still splits, phone cell or not.
  auto parked = moving_b;
  parked.location.mobility = config::Mobility::Fixed;
  assert(!oslogger::same_file_labels(parked, moving_b));

  // A reboot clears the RAM only cell. Rows fall back to GNSS or nulls.
  set(phone_line(phone_text));
  assert(config::sample_labels().location.phone_cell == cells.at8);
  assert(config::load(false));
  const auto rebooted = snapshot();
  assert(rebooted.location.phone_cell == 0);
  assert(rebooted.location.mobility == config::Mobility::Moving);
  assert(chosen(rebooted).source == LocationSource::Gnss);
  // A phone cell that is not res 8 is never published and falls through.
  auto odd = rebooted.location;
  odd.phone_cell = cells.at9;
  assert(gnss::select_location(odd, fresh_fix, now).source ==
         LocationSource::Gnss);
  odd.mobility = config::Mobility::Fixed;
  odd.cell = kConfigured;
  assert(gnss::select_location(odd, fresh_fix, now).source ==
         LocationSource::Configured);
}

// One request moves the location and the deployment id together, and a
// sample read while requests land never pairs one request's location with the
// other request's deployment id (#151). The writer stands in for the storage
// worker and the reader for collect() on the main loop.
void coherent_labels() {
  fake_platform::nvs = {};
  assert(config::load(false));
  constexpr std::string_view kCairo = "location.country=EG\n"
                                      "thing.deployment_id=cairo-07";
  constexpr std::string_view kGiza = "location.country=US\n"
                                     "thing.deployment_id=giza-01";
  const auto set = [](std::string_view lines) {
    config::Actions actions{};
    char bad_key[32]{};
    assert(config::apply_lines(lines.data(), lines.size(), bad_key,
                               sizeof bad_key, actions));
  };
  set(kCairo);
  // The writer starts only after the reader finished its first read, so the
  // first sample is always the cairo record. The reader keeps reading until it
  // has at least two samples and the writer is done, so the counts below hold
  // however the threads are scheduled.
  std::atomic<bool> started{false};
  std::atomic<bool> done{false};
  std::thread writer([&] {
    while (!started.load())
      std::this_thread::yield();
    for (int i = 0; i < 2000; ++i)
      set(i % 2 ? kCairo : kGiza);
    done.store(true);
  });
  unsigned cairo = 0, giza = 0;
  do {
    const auto labels = config::sample_labels();
    const std::string_view country = labels.location.country;
    const std::string_view deployment = labels.deployment.text.data();
    assert((country == "EG" && deployment == "cairo-07") ||
           (country == "US" && deployment == "giza-01"));
    ++(country == "EG" ? cairo : giza);
    started.store(true);
  } while (cairo + giza < 2 || !done.load());
  writer.join();
  assert(cairo > 0 && cairo + giza > 1);
}
} // namespace

int main() {
  parser();
  conversion();
  freshness();
  readback();
  source_rule();
  row_mapping();
  no_split();
  deployment_split();
  phone_cell_rule();
  coherent_labels();
  std::puts("gnss host tests passed");
}
