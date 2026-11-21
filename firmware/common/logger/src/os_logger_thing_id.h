#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <expected>
#include <span>
#include <string_view>

// The Thing id in NVS `parquet/thing` (owner decision 31, 2026-10-26): a
// random version 4 UUID in lowercase, written once on the first boot and
// read on every boot after.
//
// load_or_create() writes only when the key is not found. A stored value
// that cannot be read, has another type or fails the format check stays as
// it is, it is the owner's Thing id, and the result names the stage and the
// ESP-IDF error so the serial line and the board can show it (#225).
//
// Pure and host tested. The NVS calls are behind a Store with
//   int open()                                       // nvs_open, read-write
//   int get(std::span<char, kTextBytes> out, std::size_t &length)
//   int set(const char *text)                        // nvs_set_str
//   int commit()                                     // nvs_commit
//   void close()                                     // after a good open
// Each int is an esp_err_t, 0 is ESP_OK.
namespace oslogger::thing_id {

inline constexpr char kNamespace[] = "parquet";
inline constexpr char kKey[] = "thing";
inline constexpr std::size_t kLength = 36;
inline constexpr std::size_t kTextBytes = kLength + 1;
inline constexpr int kOk = 0;
// ESP_ERR_NVS_NOT_FOUND, os_logger.cpp checks it against the SDK value.
inline constexpr int kNotFound = 0x1102;

// Where loading the Thing id stopped. Error only, there is no Ok value.
enum class [[nodiscard]] Stage : std::uint8_t {
  Open,   // nvs_open of the namespace failed
  Read,   // nvs_get_str failed with anything but not found
  Write,  // nvs_set_str or nvs_commit of a new id failed
  Format, // the stored value is not a 36 character lowercase UUID
};

[[nodiscard]] constexpr const char *stage_name(Stage stage) {
  using enum Stage;
  switch (stage) {
  case Open:
    return "open";
  case Read:
    return "read";
  case Write:
    return "write";
  case Format:
    return "format";
  }
  return "unknown";
}

// The stage and the esp_err_t it returned. A Format error read fine, so its
// code is 0.
struct Error {
  Stage stage;
  int code;
};

// The serial line begin() prints, and boards repeat, with the stage name and
// the code. Logging stays off for the boot.
inline constexpr char kFaultLine[] =
    "PARQUET ERROR operation=thing-identity stage=%s error=%d logging=false\n";

// True for "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" with lowercase hex digits,
// the check every boot applies to the stored value.
[[nodiscard]] constexpr bool valid(std::string_view text) {
  if (text.size() != kLength)
    return false;
  for (std::size_t i = 0; i < kLength; ++i) {
    const char c = text[i];
    const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
    if (dash ? c != '-' : !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
      return false;
  }
  return true;
}

// Writes the version 4 UUID text of 16 random bytes, version and variant
// bits set.
inline void format(std::array<std::uint8_t, 16> id,
                   std::span<char, kTextBytes> text) {
  id[6] = static_cast<std::uint8_t>((id[6] & 0x0f) | 0x40);
  id[8] = static_cast<std::uint8_t>((id[8] & 0x3f) | 0x80);
  std::snprintf(
      text.data(), text.size(),
      "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
      id[0], id[1], id[2], id[3], id[4], id[5], id[6], id[7], id[8], id[9],
      id[10], id[11], id[12], id[13], id[14], id[15]);
}

// Reads the stored Thing id into `text`, or makes and stores a new one when
// the key is not found. `fill` puts 16 random bytes into a
// std::array<std::uint8_t, 16>. On any error `text` is left empty, so no
// unchecked value reaches a file name or a radio.
template <class Store, class Fill>
[[nodiscard]] std::expected<void, Error>
load_or_create(Store &store, std::span<char, kTextBytes> text, Fill &&fill) {
  const auto fail = [&](Stage stage, int code) {
    text[0] = '\0';
    return std::unexpected(Error{.stage = stage, .code = code});
  };
  text[0] = '\0';
  if (const int opened = store.open(); opened != kOk)
    return fail(Stage::Open, opened);
  std::size_t length = text.size();
  const int found = store.get(text, length);
  if (found == kNotFound) {
    std::array<std::uint8_t, 16> id{};
    fill(id);
    format(id, text);
    int wrote = store.set(text.data());
    if (wrote == kOk)
      wrote = store.commit();
    if (wrote != kOk) {
      store.close();
      return fail(Stage::Write, wrote);
    }
  } else if (found != kOk) {
    store.close();
    return fail(Stage::Read, found);
  }
  store.close();
  // A value without its NUL inside the buffer fails the check too.
  const auto end = std::find(text.begin(), text.end(), '\0');
  if (end == text.end() ||
      !valid(std::string_view{text.data(),
                              static_cast<std::size_t>(end - text.begin())}))
    return fail(Stage::Format, kOk);
  return {};
}

} // namespace oslogger::thing_id
