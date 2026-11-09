#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>

// Stateless H3 helpers. Persist only the result of published_cell(); derive
// public coordinates from that cell. A cell center is not the original GPS
// position or an administrative country lookup.
namespace opensensor::location {
using Cell = std::uint64_t;
// Every published cell is H3 res 8 (owner decision OD-2026-11-04-B, #139).
// The config key location.resolution accepts only this value.
constexpr int kPublishedResolution = 8;
constexpr std::size_t kCellTextBytes = 16; // 15 lowercase hex digits + NUL

bool valid_cell(Cell cell);
int resolution(Cell cell); // -1 for an invalid cell, including absent cell 0
bool parse_cell(const char *text, Cell &cell);
bool format_cell(Cell cell, char *text, std::size_t size);
// Maximum disclosure resolution, inclusive 0..15. A coarser input is retained
// as-is; increasing this limit cannot reconstruct finer location information.
bool coarsen(Cell input, int maximum_resolution, Cell &published);
// Why a cell cannot be published.
enum class [[nodiscard]] PublishError : std::uint8_t {
  Invalid,   // not an H3 cell, including the absent cell 0
  TooCoarse, // coarser than res 8, the finer cell cannot be restored
};
// The res 8 cell to publish for `input`. A finer cell becomes its res 8
// parent, a res 8 cell is kept and a coarser one is refused, never upsampled.
[[nodiscard]] std::expected<Cell, PublishError> published_cell(Cell input);
bool center_degrees(Cell cell, double &latitude, double &longitude);
bool center_e7(Cell cell, std::int64_t &latitude, std::int64_t &longitude);
bool from_degrees(double latitude, double longitude, int resolution,
                  Cell &cell);
// Assigned ISO 3166-1 alpha-2 code only; country is explicitly owner-declared.
// Empty input means missing to callers, and is not itself a valid country code.
bool valid_country(const char *country);
} // namespace opensensor::location
