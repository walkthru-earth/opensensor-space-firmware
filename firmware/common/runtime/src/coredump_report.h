#pragma once

// Report a saved core dump once in the LOG ring.
//
// Pure and board neutral. The ESP-IDF core dump API and the NVS record sit
// behind the borrowed Source and Store callbacks, so the host test can fake
// both. The device glue lives in log_persist.cpp.
//
// A dump is identified by its size and the SHA-256 that ESP-IDF stores at the
// end of the image. After the first COREDUMP line for a dump, its identity is
// saved and later boots stay quiet until a different dump is saved. The dump
// is never erased here. If the saved identity cannot be read, the dump is
// reported again rather than hidden.

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

namespace opensensor::coredump {

// ESP-IDF 6.1.1 always uses SHA-256 for core dumps in flash.
inline constexpr std::size_t kChecksumBytes = 32;
inline constexpr std::size_t kTaskNameBytes = 16;
// At most this many hex characters of the crashed application's ELF SHA-256
// are printed. ESP-IDF supplies CONFIG_APP_RETRIEVE_LEN_ELF_SHA - 1 of them,
// 8 with the default.
inline constexpr std::size_t kElfShaChars = 16;
// Record layout, version byte, dump bytes (u32 little endian), checksum.
inline constexpr std::uint8_t kRecordVersion = 1;
inline constexpr std::size_t kRecordBytes = 1 + 4 + kChecksumBytes;
// Below the 256 byte DebugLog::printf stack buffer.
inline constexpr std::size_t kLineBytes = 224;

using Record = std::array<std::uint8_t, kRecordBytes>;

struct Identity {
  std::uint32_t bytes = 0;
  std::array<std::uint8_t, kChecksumBytes> checksum{};
  friend bool operator==(const Identity &, const Identity &) = default;
};

// Text fields need not be NUL terminated. Any byte outside [A-Za-z0-9_.-]
// prints as '_', and the first NUL ends the field.
struct Summary {
  std::array<char, kTaskNameBytes> task{};
  std::uint32_t pc = 0;
  std::uint32_t exc_cause = 0;
  std::uint32_t exc_vaddr = 0;
  std::array<char, kElfShaChars> elf{};
};

enum class SourceError : std::uint8_t {
  Absent,      // blank partition or no core dump partition
  InvalidSize, // the size word does not describe an image
  ReadFailed,  // flash read failed
  Corrupt,     // the image checksum does not match its contents
};

enum class StoreError : std::uint8_t {
  Missing,     // nothing saved yet
  ReadFailed,  // the store could not be read
  WriteFailed, // the store could not be written
};

enum class RecordError : std::uint8_t {
  WrongVersion,
};

// Borrowed callbacks. Each one is required.
struct Source {
  void *context = nullptr;
  std::expected<Identity, SourceError> (*identity)(void *context) = nullptr;
  std::expected<void, SourceError> (*verify)(void *context) = nullptr;
  std::expected<Summary, SourceError> (*summary)(void *context) = nullptr;
};

struct Store {
  void *context = nullptr;
  std::expected<Record, StoreError> (*load)(void *context) = nullptr;
  std::expected<void, StoreError> (*save)(
      void *context,
      std::span<const std::uint8_t, kRecordBytes> record) = nullptr;
};

enum class [[nodiscard]] Decision : std::uint8_t {
  Absent,          // no dump, no line
  AlreadyReported, // same dump as the saved identity, no line
  Reported,        // a COREDUMP line was written
  Unreadable,      // the dump could not be identified, a line was written
};

struct Report {
  Decision decision = Decision::Absent;
  std::size_t length = 0; // bytes in line, 0 when nothing is written
};

[[nodiscard]] Record encode(const Identity &identity);
[[nodiscard]] std::expected<Identity, RecordError>
decode(std::span<const std::uint8_t, kRecordBytes> record);

// Decide and format. Writes at most one NUL terminated line ending in '\n'
// into `line` and saves the identity after reporting a dump.
[[nodiscard]] Report report(const Source &source, const Store &store,
                            std::span<char, kLineBytes> line);

} // namespace opensensor::coredump
