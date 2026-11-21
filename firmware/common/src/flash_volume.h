#pragma once

// Internal flash FAT volume for cardless logging (decision record 0004).
//
// The partition labelled "spiffs" is mounted through wear levelling at
// "/flash". It is formatted at most once in the life of the board. The first
// format records `osflash/formatted` = 1 in NVS, and after that a failed
// mount is an error that leaves the volume off. A first ESP_FAIL is retried
// once without a format, so a transient fault never spends the format. Owner data on the volume is
// never formatted away by a later boot.
//
// The policy below is pure and board neutral. The ESP-IDF mount, the NVS flag
// and the free space query sit behind borrowed callbacks, so the host test
// can fake them. The device glue is mount_flash_volume() in flash_volume.cpp.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

namespace opensensor::storage {

inline constexpr char kFlashMountPoint[] = "/flash";
inline constexpr char kFlashPartitionLabel[] = "spiffs";
// Archive root a board passes as oslogger::Config::fallback_directory.
inline constexpr char kFlashArchiveDirectory[] = "/flash/output";
inline constexpr char kFlashFlagNamespace[] = "osflash";
inline constexpr char kFlashFlagKey[] = "formatted";
// Room for the retry line, an ERROR line and the mount line, below the 256
// byte DebugLog::printf stack buffer.
inline constexpr std::size_t kFlashLineBytes = 224;
// ESP_FAIL, the code the FAT mount returns when f_mount fails. Only this
// failure can be cured by a format. flash_volume.cpp checks the value.
inline constexpr std::int32_t kEspFail = -1;
// WL_INVALID_HANDLE. flash_volume.cpp checks the value.
inline constexpr std::int32_t kNoWlHandle = -1;

struct FlashVolume {
  std::int32_t wl_handle = kNoWlHandle; // wl_handle_t
  std::uint64_t total_bytes = 0;        // 0 when the size query failed
  std::uint64_t free_bytes = 0;         // 0 when the size query failed
  bool formatted = false;               // true only on the boot that formatted
};

enum class [[nodiscard]] FlashMountError : std::uint8_t {
  NotFormattable,   // the mount failed in a way a format cannot cure
  AlreadyFormatted, // the mount failed and the one format is spent
  FlagUnreadable,   // the mount failed and the flag could not be read
  FormatFailed,     // the one allowed format did not give a volume
};

// A failed esp_err_t carried as an error, never ESP_OK. It has no named
// values on purpose and only reaches the log line through error_name.
enum class [[nodiscard]] FlashMountCode : std::int32_t {};

enum class [[nodiscard]] FlashFlagError : std::uint8_t {
  ReadFailed,
  WriteFailed,
};

enum class [[nodiscard]] FlashSpaceError : std::uint8_t {
  Unavailable,
};

struct FlashSpace {
  std::uint64_t total_bytes = 0;
  std::uint64_t free_bytes = 0;
};

// Borrowed callbacks. Each one is required.
struct FlashOps {
  void *context = nullptr;
  // One mount attempt. Returns the wear levelling handle. A failed attempt
  // leaves nothing mounted.
  std::expected<std::int32_t, FlashMountCode> (*mount)(
      void *context, bool format_if_mount_failed) = nullptr;
  // The stored flag value. A flag that was never written reads as 0.
  std::expected<std::uint8_t, FlashFlagError> (*load_flag)(void *context) =
      nullptr;
  // Stores the flag as 1 and commits it.
  std::expected<void, FlashFlagError> (*save_flag)(void *context) = nullptr;
  std::expected<FlashSpace, FlashSpaceError> (*space)(void *context) = nullptr;
  // Never null. Returns a static name such as "ESP_FAIL".
  const char *(*error_name)(FlashMountCode code) = nullptr;
};

// Runs the format-once policy. Writes one or two NUL terminated lines, each
// ending in '\n', into `line`. The last one is always the OS FLASH mount line.
[[nodiscard]] std::expected<FlashVolume, FlashMountError>
mount_flash_policy(const FlashOps &ops, std::span<char, kFlashLineBytes> line);

// Device glue. Mounts kFlashPartitionLabel at kFlashMountPoint. NVS must be
// initialized first. Call once at boot, before the logger starts.
[[nodiscard]] std::expected<FlashVolume, FlashMountError>
mount_flash_volume(std::span<char, kFlashLineBytes> line);

} // namespace opensensor::storage
