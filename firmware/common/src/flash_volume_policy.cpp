#include "flash_volume.h"

#include <cstdarg>
#include <cstdio>
#include <utility>

namespace opensensor::storage {
namespace {

// Appends one formatted line at `used` and keeps the buffer NUL terminated.
// A line that does not fit is cut, and the buffer still ends in a NUL.
__attribute__((format(printf, 3, 4))) void
append(std::span<char, kFlashLineBytes> line, std::size_t &used,
       const char *format, ...) {
  if (used + 1 >= line.size())
    return;
  std::va_list arguments;
  va_start(arguments, format);
  const int written =
      std::vsnprintf(line.data() + used, line.size() - used, format, arguments);
  va_end(arguments);
  if (written <= 0)
    return;
  const auto length = static_cast<std::size_t>(written);
  used = used + length < line.size() ? used + length : line.size() - 1;
}

void append_failure(const FlashOps &ops, std::span<char, kFlashLineBytes> line,
                    std::size_t &used, FlashMountCode code, bool flag_set) {
  append(line, used, "OS FLASH mount ok=0 err=%s formatted_flag=%d\n",
         ops.error_name(code), flag_set ? 1 : 0);
}

[[nodiscard]] FlashVolume mounted(const FlashOps &ops, std::int32_t handle,
                                  bool formatted) {
  FlashVolume volume{.wl_handle = handle, .formatted = formatted};
  if (const auto space = ops.space(ops.context); space) {
    volume.total_bytes = space->total_bytes;
    volume.free_bytes = space->free_bytes;
  }
  return volume;
}

void append_success(std::span<char, kFlashLineBytes> line, std::size_t &used,
                    const FlashVolume &volume) {
  append(line, used,
         "OS FLASH mount ok=1 label=%s total=%llu free=%llu formatted=%d\n",
         kFlashPartitionLabel,
         static_cast<unsigned long long>(volume.total_bytes),
         static_cast<unsigned long long>(volume.free_bytes),
         volume.formatted ? 1 : 0);
}

} // namespace

std::expected<FlashVolume, FlashMountError>
mount_flash_policy(const FlashOps &ops, std::span<char, kFlashLineBytes> line) {
  line[0] = '\0';
  std::size_t used = 0;

  const auto first = ops.mount(ops.context, false);
  if (first) {
    const FlashVolume volume = mounted(ops, *first, false);
    append_success(line, used, volume);
    return volume;
  }

  // An unreadable flag counts as set, so a read fault never formats.
  const auto flag = ops.load_flag(ops.context);
  if (!flag)
    append(line, used, "OS FLASH ERROR operation=flag-read\n");
  const bool flag_set = !flag || *flag == 1;

  if (std::to_underlying(first.error()) != kEspFail) {
    // A missing partition or no memory is not cured by a format.
    append_failure(ops, line, used, first.error(), flag_set);
    return std::unexpected(FlashMountError::NotFormattable);
  }
  if (!flag) {
    append_failure(ops, line, used, first.error(), flag_set);
    return std::unexpected(FlashMountError::FlagUnreadable);
  }
  if (flag_set) {
    append_failure(ops, line, used, first.error(), flag_set);
    return std::unexpected(FlashMountError::AlreadyFormatted);
  }

  const auto second = ops.mount(ops.context, true);
  if (!second) {
    append_failure(ops, line, used, second.error(), false);
    return std::unexpected(FlashMountError::FormatFailed);
  }
  // The volume is formatted and mounted. A flag that fails to save leaves it
  // mounted, and the next failed mount may format once more.
  if (!ops.save_flag(ops.context))
    append(line, used, "OS FLASH ERROR operation=flag-save\n");
  const FlashVolume volume = mounted(ops, *second, true);
  append_success(line, used, volume);
  return volume;
}

} // namespace opensensor::storage
