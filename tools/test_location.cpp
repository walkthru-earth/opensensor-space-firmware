// Host checks for the firmware location policy (firmware/common/location)
// over the pinned embedded-h3-location library. The library proves its own
// API in its tests. These keep the official H3 fixtures the firmware relies
// on, the res 8 publish rule, the logger mapping and the exact ISO country
// set, so a library bump that changes any of them fails here.
#include "os_location.h"
#include <numeric_sample.h>
#include <os_gnss.h>
#include <os_logger_location.h>

#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <string_view>

using namespace opensensor::location;

namespace {
// The cell coarsened to `maximum`, or 0. Every caller asserts a valid result.
Cell coarsened(Cell cell, int maximum) {
  return coarsen(cell, maximum).value_or(0);
}
int resolution_of(Cell cell) { return resolution(cell).value_or(-1); }

// The accepted country set must be exactly the ISO list in
// tools/fixtures/location/iso_alpha2.txt, copied from the firmware table the
// library replaced. Every one of the 676 uppercase pairs is checked, so a
// library bump that adds, drops or renames a code fails here.
void countries_match_fixture(const char *path) {
  std::FILE *file = std::fopen(path, "r");
  assert(file);
  std::array<bool, 26 * 26> listed{};
  int count = 0;
  std::array<char, 256> line{};
  while (std::fgets(line.data(), static_cast<int>(line.size()), file)) {
    if (line[0] == '#')
      continue;
    for (const char *at = line.data(); *at;) {
      if (*at == ' ' || *at == '\n') {
        ++at;
        continue;
      }
      assert(at[0] >= 'A' && at[0] <= 'Z' && at[1] >= 'A' && at[1] <= 'Z');
      assert(at[2] == ' ' || at[2] == '\n' || at[2] == '\0');
      bool &seen = listed[(at[0] - 'A') * 26 + (at[1] - 'A')];
      assert(!seen);
      seen = true;
      ++count;
      at += 2;
    }
  }
  std::fclose(file);
  assert(count == 249);
  int accepted = 0;
  int rejected = 0;
  for (char first = 'A'; first <= 'Z'; ++first) {
    for (char second = 'A'; second <= 'Z'; ++second) {
      const std::array<char, 3> code{first, second, '\0'};
      const bool valid = valid_country(code.data());
      assert(valid == listed[(first - 'A') * 26 + (second - 'A')]);
      ++(valid ? accepted : rejected);
    }
  }
  assert(accepted == 249 && rejected == 427);
}
} // namespace

int main(int argc, char **argv) {
  assert(argc == 2);
  countries_match_fixture(argv[1]);
  // Published official indexing API example, not a wrapper-generated vector.
  const auto indexed = from_degrees(45, 40, 2);
  assert(indexed && *indexed == 0x822d57fffffffffULL);
  assert(resolution_of(*indexed) == 2);
  const auto parsed = parse_cell("85283473FFFFFFF");
  assert(parsed && *parsed == 0x85283473fffffffULL);
  const Cell cell = *parsed;
  std::array<char, kCellTextBytes> text{};
  assert(format_cell(cell, text));
  assert(std::string_view(text.data()) == "85283473fffffff");
  // Official H3 cellToLatLng documentation example, printed to ten decimals.
  const auto center = center_degrees(cell);
  assert(center && std::abs(center->latitude - 37.3457933754) < 1e-10);
  assert(std::abs(center->longitude - -121.9763759726) < 1e-10);
  const auto e7 = center_e7(cell);
  assert(e7 && e7->latitude == 373457934 && e7->longitude == -1219763760);
  assert(coarsened(cell, 4) == 0x8428347ffffffffULL);

  for (const std::string_view bad :
       {"", "0", "085283473fffffff", "0x85283473fffffff", "+85283473fffffff",
        "85283473ffffffg", "85283473fffffff ", "85283473fffffffjunk"})
    assert(!parse_cell(bad));
  // Upstream testH3Index deleted pentagon subsequence: res1/base4/digit1.
  // Other bit mutations exercise mode, reserved/high bits, unused digits and
  // invalid used digits through the official validator, never a local decoder.
  for (Cell bad :
       {0ULL, 0x81087ffffffffffULL, cell | (1ULL << 63), cell | (1ULL << 56),
        (cell & ~(15ULL << 59)) | (2ULL << 59), cell & ~7ULL,
        cell | (7ULL << 30), (cell & ~(127ULL << 45)) | (127ULL << 45)}) {
    assert(!valid_cell(bad));
    assert(!resolution(bad) && resolution(bad).error() == Error::InvalidCell);
    assert(!coarsen(bad, 5));
    assert(!center_degrees(bad) && !center_e7(bad));
    std::array<char, kCellTextBytes> untouched{'x'};
    assert(!format_cell(bad, untouched) && untouched[0] == 'x');
  }
  assert(valid_cell(0x80c3fffffffffffULL)); // upstream res0 pentagon fixture
  assert(valid_cell(0x81083ffffffffffULL)); // valid center child of base4
  assert(!coarsen(cell, -1) && !coarsen(cell, 16));

  // Provision once at high precision, persist only the published result.
  // Increasing the limit cannot recover discarded coordinates or children.
  const auto fine_cell =
      from_degrees(37.775938728915946, -122.41795063018799, 9);
  assert(fine_cell && *fine_cell == 0x8928308280fffffULL); // quick-start SF
  const Cell fine = *fine_cell;
  // Res 8 is the only published resolution (#83, #139), about 0.74 km2.
  static_assert(kPublishedResolution == 8);
  assert(resolution_of(coarsened(fine, kPublishedResolution)) == 8);
  const Cell coarse = coarsened(fine, 5);
  assert(coarse == 0x85283083fffffffULL);
  assert(coarsened(coarse, 15) == coarse);
  const Cell lower = coarsened(coarse, 3);
  assert(resolution_of(lower) == 3 && coarsened(lower, 5) == lower);
  for (int maximum = 0; maximum <= 15; ++maximum) {
    const Cell parent = coarsened(fine, maximum);
    assert(resolution_of(parent) == (maximum < 9 ? maximum : 9));
    const auto degrees = center_degrees(parent);
    const auto fixed = center_e7(parent);
    assert(degrees && fixed);
    assert(std::abs(degrees->latitude - fixed->latitude / 1e7) <= 5.1e-8);
    assert(std::abs(degrees->longitude - fixed->longitude / 1e7) <= 5.1e-8);
  }
  const auto west = from_degrees(0, -180, 5);
  const auto east = from_degrees(0, 180, 5);
  assert(west && east && *west == *east);
  const auto antimeridian = center_degrees(*west);
  assert(antimeridian && std::abs(antimeridian->longitude) > 179 &&
         antimeridian->longitude >= -180 && antimeridian->longitude <= 180);
  for (double pole : {-90., 90.})
    assert(from_degrees(pole, 180, 15));
  for (double invalid : {std::numeric_limits<double>::quiet_NaN(),
                         std::numeric_limits<double>::infinity(), -91., 91.})
    assert(!from_degrees(invalid, 0, 5));
  for (double invalid : {std::numeric_limits<double>::quiet_NaN(),
                         std::numeric_limits<double>::infinity(), -181., 181.})
    assert(!from_degrees(0, invalid, 5));
  assert(!from_degrees(0, 0, -1) && !from_degrees(0, 0, 16));

  // published_cell() is the one gate to a published cell (#139). Finer cells
  // become their res 8 parent, res 8 is kept, coarser and invalid are refused.
  constexpr Cell kAt8 = 0x8828308281fffffULL;
  const auto finest = from_degrees(37.775938728915946, -122.41795063018799, 15);
  assert(finest);
  for (int finer = 8; finer <= 15; ++finer) {
    const Cell input = coarsened(*finest, finer);
    assert(resolution_of(input) == finer);
    const auto published = published_cell(input);
    assert(published && *published == kAt8 && resolution_of(*published) == 8);
  }
  for (int coarser = 0; coarser < 8; ++coarser) {
    const auto refused = published_cell(coarsened(fine, coarser));
    assert(!refused && refused.error() == Error::TooCoarse);
  }
  for (const Cell bad : {Cell{0}, Cell{1}, Cell{0xffffffffffffffffULL}}) {
    const auto refused = published_cell(bad);
    assert(!refused && refused.error() == Error::InvalidCell);
  }
  for (const char *code : {"EG", "US", "TW", "BQ", "SS", "GB", "AQ"})
    assert(valid_country(code));
  for (const char *code : {"", "A", "ZZ", "AA", "XK", "UK", "QM", "us", "USA"})
    assert(!valid_country(code));

  // Exercise the exact production envelope and the mapping collect() uses,
  // select_location then apply_cell, with no receiver fix. A queued snapshot
  // owns its settings; later owner changes cannot rewrite its values.
  const auto apply = [](auto &row) {
    const auto chosen =
        opensensor::gnss::select_location(row.location, std::nullopt, 0);
    return oslogger::apply_cell(row, chosen.cell, chosen.source, {0, 1, 2, 3});
  };
  config::LocationSettings current{.cell = kAt8,
                                   .resolution = 8,
                                   .country = "US",
                                   .mobility = config::Mobility::Fixed};
  oslogger::LocationSample<opensensor::NumericSample<4>> captured{};
  captured.location = current;
  assert(apply(captured));
  current.cell = 0;
  std::strcpy(current.country, "EG");
  assert(captured.location.cell == kAt8);
  assert(captured.data[0] == static_cast<std::int64_t>(kAt8) &&
         captured.data[1] == 8);
  // location_source 1, configured, and the cell centre as a packed point.
  assert(captured.data[2] == 1 && captured.valid[2] && captured.valid[3]);
  assert(captured.data[3] == telemetry::pack_point_e7(-1224182710, 377735151));
  // A snapshot cell that is not res 8 is never published, finer or coarser.
  for (const Cell other : {fine, coarse}) {
    captured.location.cell = other;
    assert(!apply(captured));
    for (const auto present : captured.valid)
      assert(!present);
  }
  captured.location = {};
  assert(!apply(captured));
  for (const auto present : captured.valid)
    assert(!present);
}
