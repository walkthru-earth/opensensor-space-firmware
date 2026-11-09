#include "os_location.h"
#include <numeric_sample.h>
#include <os_logger_location.h>

#include <cassert>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <limits>

using namespace opensensor::location;

int main() {
  // Published official indexing API example, not a wrapper-generated vector.
  Cell cell = 0;
  assert(from_degrees(45, 40, 2, cell));
  assert(cell == 0x822d57fffffffffULL);
  assert(resolution(cell) == 2);
  assert(parse_cell("85283473FFFFFFF", cell));
  assert(cell == 0x85283473fffffffULL);
  char text[kCellTextBytes]{};
  assert(format_cell(cell, text, sizeof(text)));
  assert(std::strcmp(text, "85283473fffffff") == 0);
  double lat = 0, lon = 0;
  assert(center_degrees(cell, lat, lon));
  // Official H3 cellToLatLng documentation example, printed to ten decimals.
  assert(std::abs(lat - 37.3457933754) < 1e-10);
  assert(std::abs(lon - -121.9763759726) < 1e-10);
  std::int64_t lat_e7 = 0, lon_e7 = 0;
  assert(center_e7(cell, lat_e7, lon_e7));
  assert(lat_e7 == 373457934 && lon_e7 == -1219763760);
  Cell parent = 0;
  assert(coarsen(cell, 4, parent));
  assert(parent == 0x8428347ffffffffULL);

  const Cell unchanged = cell;
  for (const char *bad :
       {"", "0", "085283473fffffff", "0x85283473fffffff", "+85283473fffffff",
        "85283473ffffffg", "85283473fffffff ", "85283473fffffffjunk"}) {
    assert(!parse_cell(bad, cell));
    assert(cell == unchanged);
  }
  assert(!parse_cell(nullptr, cell));
  assert(!format_cell(cell, nullptr, sizeof(text)));
  assert(!format_cell(cell, text, sizeof(text) - 1));
  // Upstream testH3Index deleted pentagon subsequence: res1/base4/digit1.
  // Other bit mutations exercise mode, reserved/high bits, unused digits and
  // invalid used digits through the official validator, never a local decoder.
  for (Cell bad :
       {0ULL, 0x81087ffffffffffULL, cell | (1ULL << 63), cell | (1ULL << 56),
        (cell & ~(15ULL << 59)) | (2ULL << 59), cell & ~7ULL,
        cell | (7ULL << 30), (cell & ~(127ULL << 45)) | (127ULL << 45)}) {
    assert(!valid_cell(bad));
    assert(resolution(bad) == -1);
    parent = unchanged;
    assert(!coarsen(bad, 5, parent));
    assert(parent == unchanged);
    lat = 111;
    lon = 222;
    assert(!center_degrees(bad, lat, lon));
    assert(lat == 111 && lon == 222);
    lat_e7 = 111;
    lon_e7 = 222;
    assert(!center_e7(bad, lat_e7, lon_e7));
    assert(lat_e7 == 111 && lon_e7 == 222);
  }
  assert(valid_cell(0x80c3fffffffffffULL)); // upstream res0 pentagon fixture
  assert(valid_cell(0x81083ffffffffffULL)); // valid center child of base4
  assert(!coarsen(cell, -1, parent));
  assert(!coarsen(cell, 16, parent));

  // Provision once at high precision; persist only published result. Increasing
  // the limit cannot recover discarded coordinates or child indexes.
  Cell fine = 0, coarse = 0, raised = 0, lower = 0;
  assert(from_degrees(37.775938728915946, -122.41795063018799, 9, fine));
  assert(fine == 0x8928308280fffffULL); // official quick-start SF fixture
  // Res 8 is the only published resolution (#83, #139), about 0.74 km2.
  static_assert(kPublishedResolution == 8);
  assert(coarsen(fine, kPublishedResolution, coarse) &&
         resolution(coarse) == 8);
  assert(coarsen(fine, 5, coarse));
  assert(coarse == 0x85283083fffffffULL);
  assert(coarsen(coarse, 15, raised) && raised == coarse);
  assert(coarsen(coarse, 3, lower) && resolution(lower) == 3);
  assert(coarsen(lower, 5, raised) && raised == lower);
  for (int maximum = 0; maximum <= 15; ++maximum) {
    assert(coarsen(fine, maximum, parent));
    assert(resolution(parent) == (maximum < 9 ? maximum : 9));
    assert(center_degrees(parent, lat, lon));
    assert(center_e7(parent, lat_e7, lon_e7));
    assert(std::abs(lat - lat_e7 / 1e7) <= 5.1e-8);
    assert(std::abs(lon - lon_e7 / 1e7) <= 5.1e-8);
  }
  Cell west = 0, east = 0;
  assert(from_degrees(0, -180, 5, west));
  assert(from_degrees(0, 180, 5, east));
  assert(west == east);
  assert(center_degrees(west, lat, lon));
  assert(std::abs(lon) > 179 && lon >= -180 && lon <= 180);
  for (double pole : {-90., 90.})
    assert(from_degrees(pole, 180, 15, cell));
  for (double invalid : {std::numeric_limits<double>::quiet_NaN(),
                         std::numeric_limits<double>::infinity(), -91., 91.})
    assert(!from_degrees(invalid, 0, 5, cell));
  for (double invalid : {std::numeric_limits<double>::quiet_NaN(),
                         std::numeric_limits<double>::infinity(), -181., 181.})
    assert(!from_degrees(0, invalid, 5, cell));
  assert(!from_degrees(0, 0, -1, cell));
  assert(!from_degrees(0, 0, 16, cell));
  // published_cell() is the one gate to a published cell (#139). Finer cells
  // become their res 8 parent, res 8 is kept, coarser and invalid are refused.
  constexpr Cell kAt8 = 0x8828308281fffffULL;
  Cell finest = 0;
  assert(from_degrees(37.775938728915946, -122.41795063018799, 15, finest));
  for (int finer = 8; finer <= 15; ++finer) {
    Cell input = 0;
    assert(coarsen(finest, finer, input) && resolution(input) == finer);
    const auto published = published_cell(input);
    assert(published && *published == kAt8 && resolution(*published) == 8);
  }
  for (int coarser = 0; coarser < 8; ++coarser) {
    assert(coarsen(fine, coarser, parent));
    const auto refused = published_cell(parent);
    assert(!refused && refused.error() == PublishError::TooCoarse);
  }
  for (const Cell bad : {Cell{0}, Cell{1}, Cell{0xffffffffffffffffULL}}) {
    const auto refused = published_cell(bad);
    assert(!refused && refused.error() == PublishError::Invalid);
  }
  for (const char *code : {"EG", "US", "TW", "BQ", "SS", "GB", "AQ"})
    assert(valid_country(code));
  for (const char *code : {"", "A", "ZZ", "AA", "XK", "UK", "QM", "us", "USA"})
    assert(!valid_country(code));
  assert(!valid_country(nullptr));
  // Exercise the exact production envelope and mapping helper. A queued
  // snapshot owns its settings; later owner changes cannot rewrite its values.
  config::LocationSettings current{.cell = kAt8,
                                   .resolution = 8,
                                   .country = "US",
                                   .mobility = config::Mobility::Fixed};
  oslogger::LocationSample<opensensor::NumericSample<4>> captured{};
  captured.location = current;
  assert(oslogger::apply_location(captured, captured.location, {0, 1, 2, 3}));
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
    assert(
        !oslogger::apply_location(captured, captured.location, {0, 1, 2, 3}));
    for (const auto present : captured.valid)
      assert(!present);
  }
  captured.location = {};
  assert(!oslogger::apply_location(captured, captured.location, {0, 1, 2, 3}));
  for (const auto present : captured.valid)
    assert(!present);
}
