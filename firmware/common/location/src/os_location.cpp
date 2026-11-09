#include "os_location.h"
#include "iso_alpha2.h"
#include <h3api.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace opensensor::location {
bool valid_cell(Cell cell) { return isValidCell(cell) != 0; }

int resolution(Cell cell) {
  return valid_cell(cell) ? getResolution(cell) : -1;
}

bool parse_cell(const char *text, Cell &cell) {
  // Constrain input before calling H3's numeric parser (which accepts numeric
  // prefixes). Valid cells have 15 hex digits; reject
  // prefixes/signs/whitespace.
  if (!text || std::strlen(text) != kCellTextBytes - 1)
    return false;
  for (const char *p = text; *p; ++p)
    if (!(*p >= '0' && *p <= '9') && !(*p >= 'a' && *p <= 'f') &&
        !(*p >= 'A' && *p <= 'F'))
      return false;
  H3Index parsed = 0;
  if (stringToH3(text, &parsed) != E_SUCCESS || !valid_cell(parsed))
    return false;
  cell = parsed;
  return true;
}

bool format_cell(Cell cell, char *text, std::size_t size) {
  if (!text || size < kCellTextBytes || !valid_cell(cell))
    return false;
  // Upstream reserves 17 bytes for arbitrary uint64 indexes; a validated
  // cell needs only 15 digits. Keep the public canonical buffer size at 16.
  char canonical[17]{};
  if (h3ToString(cell, canonical, sizeof(canonical)) != E_SUCCESS)
    return false;
  std::memcpy(text, canonical, kCellTextBytes);
  return true;
}

bool coarsen(Cell input, int maximum_resolution, Cell &published) {
  if (maximum_resolution < 0 || maximum_resolution > 15 || !valid_cell(input))
    return false;
  H3Index parent = 0;
  if (cellToParent(input, std::min(maximum_resolution, getResolution(input)),
                   &parent) != E_SUCCESS ||
      !valid_cell(parent))
    return false;
  published = parent;
  return true;
}

std::expected<Cell, PublishError> published_cell(Cell input) {
  if (!valid_cell(input))
    return std::unexpected(PublishError::Invalid);
  if (getResolution(input) < kPublishedResolution)
    return std::unexpected(PublishError::TooCoarse);
  Cell parent = 0;
  if (!coarsen(input, kPublishedResolution, parent))
    return std::unexpected(PublishError::Invalid);
  return parent;
}

bool center_degrees(Cell cell, double &latitude, double &longitude) {
  if (!valid_cell(cell))
    return false;
  LatLng center{};
  if (cellToLatLng(cell, &center) != E_SUCCESS)
    return false;
  latitude = radsToDegs(center.lat);
  longitude = radsToDegs(center.lng);
  return true;
}

bool center_e7(Cell cell, std::int64_t &latitude, std::int64_t &longitude) {
  double lat, lon;
  if (!center_degrees(cell, lat, lon))
    return false;
  latitude = std::llround(lat * 10000000.0);
  longitude = std::llround(lon * 10000000.0);
  return true;
}

bool from_degrees(double latitude, double longitude, int res, Cell &cell) {
  if (!std::isfinite(latitude) || !std::isfinite(longitude) || latitude < -90 ||
      latitude > 90 || longitude < -180 || longitude > 180 || res < 0 ||
      res > 15)
    return false;
  const LatLng coordinates{degsToRads(latitude), degsToRads(longitude)};
  H3Index indexed = 0;
  if (latLngToCell(&coordinates, res, &indexed) != E_SUCCESS ||
      !valid_cell(indexed))
    return false;
  cell = indexed;
  return true;
}

bool valid_country(const char *country) {
  if (!country || country[0] < 'A' || country[0] > 'Z' || country[1] < 'A' ||
      country[1] > 'Z' || country[2] != '\0')
    return false;
  for (std::size_t i = 0; i + 1 < sizeof(kIsoAlpha2) - 1; i += 3)
    if (kIsoAlpha2[i] == country[0] && kIsoAlpha2[i + 1] == country[1])
      return true;
  return false;
}
} // namespace opensensor::location
