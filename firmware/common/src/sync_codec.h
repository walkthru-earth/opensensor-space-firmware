#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <span>
#include <string_view>

#if defined(ESP_PLATFORM) || defined(OS_SYNC_ROM_CRC)
#include <esp_rom_crc.h>
#endif

namespace opensensor {
namespace sync {
inline void put_u16(std::uint8_t *out, std::uint16_t value) {
  out[0] = value & 0xff;
  out[1] = value >> 8;
}
inline void put_u32(std::uint8_t *out, std::uint32_t value) {
  for (unsigned i = 0; i < 4; ++i)
    out[i] = (value >> (8 * i)) & 0xff;
}
inline void put_i64(std::uint8_t *out, std::int64_t value) {
  const auto bits = static_cast<std::uint64_t>(value);
  for (unsigned i = 0; i < 8; ++i)
    out[i] = (bits >> (8 * i)) & 0xff;
}
inline std::uint16_t get_u16(const std::uint8_t *in) {
  return static_cast<std::uint16_t>(in[0] | (in[1] << 8));
}
inline std::uint32_t get_u32(const std::uint8_t *in) {
  return std::uint32_t(in[0]) | (std::uint32_t(in[1]) << 8) |
         (std::uint32_t(in[2]) << 16) | (std::uint32_t(in[3]) << 24);
}
inline std::int64_t get_i64(const std::uint8_t *in) {
  std::uint64_t bits = 0;
  for (unsigned i = 0; i < 8; ++i)
    bits |= std::uint64_t(in[i]) << (8 * i);
  return static_cast<std::int64_t>(bits);
}

// CRC-32/ISO-HDLC running state: initialize to 0xffffffff, then XOR the final
// state with 0xffffffff. Chunk boundaries do not alter the result.
inline std::uint32_t crc_update(std::uint32_t crc, const std::uint8_t *data,
                                std::size_t size) {
  for (std::size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (unsigned bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
  }
  return crc;
}

// Same running state as crc_update. Device builds use the ESP32-S3 ROM table
// routine, which inverts its state on entry and exit (esp_rom_crc.h). Host
// builds keep the bitwise loop. tools/test_common.py pins both to the same
// golden vectors.
inline std::uint32_t crc32_update(std::uint32_t crc, const std::uint8_t *data,
                                  std::size_t size) {
#if defined(ESP_PLATFORM) || defined(OS_SYNC_ROM_CRC)
  while (size) {
    const auto part = size > 0x10000 ? std::uint32_t(0x10000)
                                     : static_cast<std::uint32_t>(size);
    crc = ~esp_rom_crc32_le(~crc, data, part);
    data += part;
    size -= part;
  }
  return crc;
#else
  return crc_update(crc, data, size);
#endif
}

// Relative finalized-file names accepted by the sync protocol. Mount roots
// and legacy routing belong to the archive owner, not this validator.
inline bool safe_parquet_name(const char *name, std::size_t max_length = 384) {
  if (!name)
    return false;
  const std::size_t length = std::strlen(name);
  if (length < 9 || length > max_length || name[0] == '/' ||
      std::strcmp(name + length - 8, ".parquet") != 0)
    return false;
  for (std::size_t i = 0; i < length; ++i)
    if (!((name[i] >= '0' && name[i] <= '9') ||
          (name[i] >= 'a' && name[i] <= 'z') || name[i] == '-' ||
          name[i] == '.' || name[i] == '_' || name[i] == '=' || name[i] == '/'))
      return false;
  return std::strstr(name, "..") == nullptr &&
         std::strstr(name, "//") == nullptr;
}

namespace detail {
constexpr bool decimal(std::string_view text) {
  if (text.empty())
    return false;
  for (const char c : text)
    if (c < '0' || c > '9')
      return false;
  return true;
}

// Takes "<key><count digits>/" off the front of rest.
constexpr bool take_field(std::string_view &rest, std::string_view key,
                          std::size_t count) {
  if (!rest.starts_with(key) || rest.size() < key.size() + count + 1 ||
      !decimal(rest.substr(key.size(), count)) ||
      rest[key.size() + count] != '/')
    return false;
  rest.remove_prefix(key.size() + count + 1);
  return true;
}

// Takes "<digits><end>" off the front of rest.
constexpr bool take_number(std::string_view &rest, char end) {
  const auto at = rest.find(end);
  if (at == std::string_view::npos || !decimal(rest.substr(0, at)))
    return false;
  rest.remove_prefix(at + 1);
  return true;
}
} // namespace detail

// The shape of a dated finalized name, the only kind with a STAC sidecar:
// thing=<id>/year=YYYY/month=MM/day=DD/data_HHMM_<32 hex>_<n>-<n>-<n>.parquet.
// The file part is the stac::item_id rule. Unsynced, flash-conflict,
// quarantine and legacy-parquet/ names are never dated. The charset and
// traversal rules are safe_parquet_name's, see dated_parquet_name.
constexpr bool dated_parquet_shape(std::string_view name) {
  constexpr std::string_view kThing = "thing=", kData = "data_",
                             kSuffix = ".parquet";
  if (!name.starts_with(kThing) || !name.ends_with(kSuffix))
    return false;
  name.remove_suffix(kSuffix.size());
  auto rest = name.substr(kThing.size());
  const auto slash = rest.find('/');
  if (slash == 0 || slash == std::string_view::npos)
    return false;
  rest.remove_prefix(slash + 1);
  if (!detail::take_field(rest, "year=", 4) ||
      !detail::take_field(rest, "month=", 2) ||
      !detail::take_field(rest, "day=", 2) || !rest.starts_with(kData))
    return false;
  rest.remove_prefix(kData.size());
  if (rest.size() < 4 + 1 + 32 + 1 || !detail::decimal(rest.substr(0, 4)) ||
      rest[4] != '_' || rest[37] != '_')
    return false;
  for (const char c : rest.substr(5, 32))
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
      return false;
  rest.remove_prefix(38);
  return detail::take_number(rest, '-') && detail::take_number(rest, '-') &&
         detail::decimal(rest);
}

[[nodiscard]] inline bool dated_parquet_name(const char *name) {
  return safe_parquet_name(name) && dated_parquet_shape(name);
}

// The STAC sidecar of a dated finalized Parquet file is "<stem>.json" beside
// "<stem>.parquet" (#142). LIST and OPEN serve it next to its Parquet (#143).
inline constexpr std::string_view kParquetSuffix = ".parquet";
inline constexpr std::string_view kSidecarSuffix = ".json";
// The largest sidecar the logger writes, stac::kMaxItemBytes. A copy keeps
// this header free of the STAC component. os_logger.cpp and
// tools/test_stac_item.cpp check that the two agree.
inline constexpr std::uint32_t kMaxSidecarBytes = 426832;
// "{}", the smallest object.
inline constexpr std::uint32_t kMinSidecarBytes = 2;

enum class [[nodiscard]] SidecarNameError : std::uint8_t {
  NotSidecar, // not the sidecar of a dated finalized Parquet name
  TooLong,    // out cannot hold the name and its NUL
};

namespace detail {
inline std::expected<std::size_t, SidecarNameError>
swap_suffix(std::string_view name, std::string_view from, std::string_view to,
            std::span<char> out) {
  name.remove_suffix(from.size());
  const std::size_t length = name.size() + to.size();
  if (length >= out.size())
    return std::unexpected(SidecarNameError::TooLong);
  std::memcpy(out.data(), name.data(), name.size());
  std::memcpy(out.data() + name.size(), to.data(), to.size());
  out[length] = '\0';
  return length;
}
} // namespace detail

// Writes "<stem>.json" for a dated Parquet name into out, NUL terminated, and
// returns its length.
[[nodiscard]] inline std::expected<std::size_t, SidecarNameError>
parquet_sidecar_name(const char *parquet, std::span<char> out) {
  if (!dated_parquet_name(parquet))
    return std::unexpected(SidecarNameError::NotSidecar);
  return detail::swap_suffix(parquet, kParquetSuffix, kSidecarSuffix, out);
}

// Writes "<stem>.parquet" for a sidecar name into out, NUL terminated, and
// returns its length. Only "<stem>.json" whose "<stem>.parquet" is a dated
// finalized name passes, so never a ".json.partial", an unsynced, conflict,
// quarantine or legacy name, nor a traversal.
[[nodiscard]] inline std::expected<std::size_t, SidecarNameError>
sidecar_parquet_name(const char *sidecar, std::span<char> out,
                     std::size_t max_length = 384) {
  if (!sidecar)
    return std::unexpected(SidecarNameError::NotSidecar);
  const std::string_view name{sidecar};
  if (!name.ends_with(kSidecarSuffix) || name.size() <= kSidecarSuffix.size() ||
      name.size() - kSidecarSuffix.size() + kParquetSuffix.size() > max_length)
    return std::unexpected(SidecarNameError::NotSidecar);
  const auto length =
      detail::swap_suffix(name, kSidecarSuffix, kParquetSuffix, out);
  if (!length)
    return length;
  if (!safe_parquet_name(out.data(), max_length) ||
      !dated_parquet_shape({out.data(), *length}))
    return std::unexpected(SidecarNameError::NotSidecar);
  return length;
}

[[nodiscard]] inline bool safe_sidecar_name(const char *name) {
  char parquet[385];
  return sidecar_parquet_name(name, parquet).has_value();
}

// Structural completion of a sidecar, the counterpart of parquet_complete.
// The writer streams one canonical JSON object, "{" first and "}" last with
// no whitespace around it (tools/test_stac_item.cpp pins that), and renames
// it into place only once it is whole.
[[nodiscard]] constexpr bool
sidecar_complete(std::uint64_t size, std::uint8_t first, std::uint8_t last) {
  return size >= kMinSidecarBytes && size <= kMaxSidecarBytes && first == '{' &&
         last == '}';
}

// Structural completion only; reader conformance remains a separate test.
// `head` is the first four bytes, `tail` the final eight (footer size + PAR1).
inline bool parquet_complete(std::uint64_t size, const std::uint8_t *head,
                             const std::uint8_t *tail,
                             std::uint64_t max_size = 1048576) {
  if (!head || !tail || size < 12 || size > max_size)
    return false;
  const std::uint32_t footer = get_u32(tail);
  return std::memcmp(head, "PAR1", 4) == 0 &&
         std::memcmp(tail + 4, "PAR1", 4) == 0 && footer > 0 &&
         footer <= size - 12;
}
} // namespace sync
} // namespace opensensor
