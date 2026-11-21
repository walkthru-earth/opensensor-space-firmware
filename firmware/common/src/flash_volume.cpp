#include "flash_volume.h"

#include "archive_files.h"

#include <diskio_wl.h>
#include <esp_err.h>
#include <esp_vfs_fat.h>
#include <nvs.h>
#include <wear_levelling.h>

namespace opensensor::storage {
namespace {

static_assert(ESP_FAIL == kEspFail);
static_assert(WL_INVALID_HANDLE == kNoWlHandle);
static_assert(sizeof(wl_handle_t) == sizeof(std::int32_t));
// The FatFs lock table is shared by the card and this volume. A build
// directory sdkconfig made before the raise keeps the old value, so delete it.
static_assert(FF_FS_LOCK == 0 || FF_FS_LOCK >= kMinFsLock,
              "CONFIG_FATFS_FS_LOCK must cover the card and flash files");

std::expected<std::int32_t, FlashMountCode> mount_once(void *, bool format) {
  const esp_vfs_fat_mount_config_t config{
      .format_if_mount_failed = format,
      .max_files = static_cast<int>(kMaxOpenFiles),
      .allocation_unit_size = CONFIG_WL_SECTOR_SIZE,
      .disk_status_check_enable = false,
      .use_one_fat = false,
  };
  wl_handle_t handle = WL_INVALID_HANDLE;
  const esp_err_t result = esp_vfs_fat_spiflash_mount_rw_wl(
      kFlashMountPoint, kFlashPartitionLabel, &config, &handle);
  if (result == ESP_OK)
    return static_cast<std::int32_t>(handle);
  // ESP-IDF 6.1 leaves the wear levelling instance mounted when the FAT
  // layer fails after wl_mount. Release it so a retry does not open a second
  // instance on the same partition. wl_mount resets the handle to
  // WL_INVALID_HANDLE on its own failure.
  if (handle != WL_INVALID_HANDLE) {
    ff_diskio_clear_pdrv_wl(handle);
    (void)wl_unmount(handle);
  }
  return std::unexpected(static_cast<FlashMountCode>(result));
}

std::expected<std::uint8_t, FlashFlagError> load_flag(void *) {
  nvs_handle_t handle = 0;
  const esp_err_t opened = nvs_open(kFlashFlagNamespace, NVS_READONLY, &handle);
  if (opened == ESP_ERR_NVS_NOT_FOUND)
    return 0;
  if (opened != ESP_OK)
    return std::unexpected(FlashFlagError::ReadFailed);
  std::uint8_t value = 0;
  const esp_err_t read = nvs_get_u8(handle, kFlashFlagKey, &value);
  nvs_close(handle);
  if (read == ESP_ERR_NVS_NOT_FOUND)
    return 0;
  if (read != ESP_OK)
    return std::unexpected(FlashFlagError::ReadFailed);
  return value;
}

std::expected<void, FlashFlagError> save_flag(void *) {
  nvs_handle_t handle = 0;
  if (nvs_open(kFlashFlagNamespace, NVS_READWRITE, &handle) != ESP_OK)
    return std::unexpected(FlashFlagError::WriteFailed);
  const bool saved = nvs_set_u8(handle, kFlashFlagKey, 1) == ESP_OK &&
                     nvs_commit(handle) == ESP_OK;
  nvs_close(handle);
  if (!saved)
    return std::unexpected(FlashFlagError::WriteFailed);
  return {};
}

// Same query and sanity check as fat_free_bytes in fat_volume.h, which this
// file does not include because that header is for the card volume.
std::expected<FlashSpace, FlashSpaceError> space(void *) {
  FlashSpace bytes{};
  if (esp_vfs_fat_info(kFlashMountPoint, &bytes.total_bytes,
                       &bytes.free_bytes) != ESP_OK ||
      bytes.free_bytes > bytes.total_bytes)
    return std::unexpected(FlashSpaceError::Unavailable);
  return bytes;
}

const char *error_name(FlashMountCode code) {
  return esp_err_to_name(static_cast<esp_err_t>(code));
}

} // namespace

std::expected<FlashVolume, FlashMountError>
mount_flash_volume(std::span<char, kFlashLineBytes> line) {
  const FlashOps ops{.context = nullptr,
                     .mount = mount_once,
                     .load_flag = load_flag,
                     .save_flag = save_flag,
                     .space = space,
                     .error_name = error_name};
  return mount_flash_policy(ops, line);
}

std::expected<FlashSpace, FlashSpaceError> flash_volume_space() {
  return space(nullptr);
}

} // namespace opensensor::storage
