#pragma once

#include <cstddef>

// FatFs descriptor budget for the archive card and the internal flash
// fallback volume, shared by every board so the mounts, the FS_LOCK table and
// the archive session cannot drift apart.
namespace opensensor::storage {
// Telemetry and codec-test writers.
constexpr std::size_t kWriterFiles = 2;
// One short-lived stream: the finalize check, serial `parquet get` or an OPEN
// replacing a slot's stream.
constexpr std::size_t kTransientFiles = 1;
// Persistent archive read streams: BLE plus each LAN peer
// (ossync::ArchiveSession::kReadHandles).
constexpr std::size_t kArchiveReadHandles = 4;
// Mount max_files, with one spare.
constexpr std::size_t kMaxOpenFiles =
    kWriterFiles + kTransientFiles + kArchiveReadHandles + 1;
// FF_FS_LOCK also counts open directories. The archive walk keeps one open
// per level, depth 0 through 6 (os_logger_directory.h), next to any open
// files. The serial archive reset keeps up to 9 open, depth 0 through 8
// (os_logger_archive_reset.h). It fits because the reset first closes the
// writer files and every archive read stream, so the walk has the table to
// itself.
constexpr std::size_t kArchiveWalkDirectories = 7;
// Copying a flash fallback file back to the card (decision record 0004)
// holds one source on the flash volume and one destination on the card.
// Both mounts share one FatFs lock table, so the pair adds to it. Each
// mount's max_files stays kMaxOpenFiles. On flash the source sits next to at
// most the telemetry writer, one transient stream and the read handles. On
// the card the destination uses the one spare slot of kMaxOpenFiles.
constexpr std::size_t kCopyBackFiles = 2;
constexpr std::size_t kMinFsLock =
    kMaxOpenFiles + kCopyBackFiles + kArchiveWalkDirectories;
} // namespace opensensor::storage
