#pragma once

#include "archive_files.h"

#include <diskio_sdmmc.h>
#include <esp_vfs_fat.h>
#include <ff.h>
#include <sdmmc_cmd.h>

#include <cstdint>

namespace opensensor::storage {

// A build directory sdkconfig made before tools/idf-sdkconfig.defaults raised
// the value keeps the old one. Delete that sdkconfig to regenerate it.
static_assert(FF_FS_LOCK == 0 || FF_FS_LOCK >= kMinFsLock,
              "CONFIG_FATFS_FS_LOCK must cover archive files and the walk");

// Mount options shared by both boards. Never formats. max_files covers the
// writers, one transient stream and every persistent archive read handle.
inline esp_vfs_fat_mount_config_t archive_mount_config() {
  esp_vfs_fat_mount_config_t mount{};
  mount.format_if_mount_failed = false;
  mount.max_files = static_cast<int>(kMaxOpenFiles);
  return mount;
}

// Bytes per FAT allocation unit of the volume mounted from `card`, or 0 when
// unknown. Every file grows in whole clusters, so the space governor logs it
// at mount. The caller serializes card access.
inline std::uint32_t fat_cluster_bytes(sdmmc_card_t *card) {
  if (!card)
    return 0;
  const BYTE drive_number = ff_diskio_get_pdrv_card(card);
  if (drive_number == 0xFF)
    return 0;
  const char drive[] = {static_cast<char>('0' + drive_number), ':', '\0'};
  DWORD free_clusters = 0;
  FATFS *volume = nullptr;
  if (f_getfree(drive, &free_clusters, &volume) != FR_OK || !volume)
    return 0;
#if FF_MAX_SS != FF_MIN_SS
  const std::uint32_t sector_bytes = volume->ssize;
#else
  const std::uint32_t sector_bytes = FF_MAX_SS;
#endif
  return static_cast<std::uint32_t>(volume->csize) * sector_bytes;
}

// f_getlabel needs CONFIG_FATFS_USE_LABEL from tools/idf-sdkconfig.defaults.
// An older build directory sdkconfig keeps the old value, so delete it.
#if !defined(CONFIG_FATFS_USE_LABEL) || !CONFIG_FATFS_USE_LABEL
#error "CONFIG_FATFS_USE_LABEL must be set for the LIST_SINCE mount epoch"
#endif

// The volume serial number of the FAT volume mounted from `card`, or 0 when
// unknown. A format writes a new one, so LIST_SINCE uses it as the mount
// epoch that tells one card from another. The caller serializes card access.
inline std::uint32_t fat_volume_serial(sdmmc_card_t *card) {
  if (!card)
    return 0;
  const BYTE drive_number = ff_diskio_get_pdrv_card(card);
  if (drive_number == 0xFF)
    return 0;
  const char drive[] = {static_cast<char>('0' + drive_number), ':', '\0'};
  DWORD serial = 0;
  if (f_getlabel(drive, nullptr, &serial) != FR_OK)
    return 0;
  return static_cast<std::uint32_t>(serial);
}

// True when `card` still answers SEND_STATUS (SD CMD13). A pulled card fails
// it. It reads no sector and never touches FatFs. The caller serializes card
// access.
inline bool card_answers(sdmmc_card_t *card) {
  return card && sdmmc_get_status(card) == ESP_OK;
}

// Fresh free space of the volume at `base`. FatFs keeps the free cluster
// count current as files grow; a remount re-reads it after an external edit.
inline bool fat_free_bytes(const char *base, std::uint64_t &free_bytes) {
  std::uint64_t total = 0, free = 0;
  if (esp_vfs_fat_info(base, &total, &free) != ESP_OK || free > total)
    return false;
  free_bytes = free;
  return true;
}

} // namespace opensensor::storage
