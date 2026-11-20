#include "../firmware/common/src/flash_volume.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
namespace st = opensensor::storage;

constexpr std::int32_t kEspErrNotFound = 0x105;
constexpr std::int32_t kEspErrNoMem = 0x101;

// One scripted result per mount call, in order.
struct FakeDevice {
  std::vector<std::expected<std::int32_t, st::FlashMountCode>> mounts{};
  std::vector<bool> format_args{};
  std::expected<std::uint8_t, st::FlashFlagError> flag = std::uint8_t{0};
  bool save_fails = false;
  int saves = 0;
  int loads = 0;
  std::expected<st::FlashSpace, st::FlashSpaceError> space =
      st::FlashSpace{.total_bytes = 4063232, .free_bytes = 4059136};
};

std::expected<st::FlashVolume, st::FlashMountError>
run(FakeDevice &device, std::array<char, st::kFlashLineBytes> &line) {
  const st::FlashOps ops{
      .context = &device,
      .mount = [](void *context, bool format)
          -> std::expected<std::int32_t, st::FlashMountCode> {
        auto &d = *static_cast<FakeDevice *>(context);
        const std::size_t call = d.format_args.size();
        d.format_args.push_back(format);
        assert(call < d.mounts.size());
        return d.mounts[call];
      },
      .load_flag =
          [](void *context) {
            auto &d = *static_cast<FakeDevice *>(context);
            ++d.loads;
            return d.flag;
          },
      .save_flag =
          [](void *context) -> std::expected<void, st::FlashFlagError> {
        auto &d = *static_cast<FakeDevice *>(context);
        if (d.save_fails)
          return std::unexpected(st::FlashFlagError::WriteFailed);
        ++d.saves;
        d.flag = std::uint8_t{1};
        return {};
      },
      .space =
          [](void *context) {
            return static_cast<FakeDevice *>(context)->space;
          },
      .error_name =
          [](st::FlashMountCode code) {
            switch (std::to_underlying(code)) {
            case st::kEspFail:
              return "ESP_FAIL";
            case kEspErrNotFound:
              return "ESP_ERR_NOT_FOUND";
            case kEspErrNoMem:
              return "ESP_ERR_NO_MEM";
            default:
              return "UNKNOWN";
            }
          }};
  return st::mount_flash_policy(ops, line);
}

std::expected<std::int32_t, st::FlashMountCode> ok(std::int32_t handle) {
  return handle;
}

std::expected<std::int32_t, st::FlashMountCode> fail(std::int32_t code) {
  return std::unexpected(static_cast<st::FlashMountCode>(code));
}

std::string_view text(const std::array<char, st::kFlashLineBytes> &line) {
  return line.data();
}

void mounts_without_format() {
  FakeDevice device{.mounts = {ok(0)}};
  std::array<char, st::kFlashLineBytes> line{};
  const auto volume = run(device, line);
  assert(volume);
  assert(volume->wl_handle == 0);
  assert(volume->total_bytes == 4063232);
  assert(volume->free_bytes == 4059136);
  assert(!volume->formatted);
  assert(device.format_args == std::vector<bool>{false});
  assert(device.loads == 0);
  assert(device.saves == 0);
  assert(text(line) == "OS FLASH mount ok=1 label=spiffs total=4063232 "
                       "free=4059136 formatted=0\n");
}

void formats_once_and_sets_flag() {
  FakeDevice device{.mounts = {fail(st::kEspFail), ok(2)}};
  std::array<char, st::kFlashLineBytes> line{};
  const auto volume = run(device, line);
  assert(volume);
  assert(volume->wl_handle == 2);
  assert(volume->formatted);
  assert((device.format_args == std::vector<bool>{false, true}));
  assert(device.saves == 1);
  assert(device.flag && *device.flag == 1);
  assert(text(line) == "OS FLASH mount ok=1 label=spiffs total=4063232 "
                       "free=4059136 formatted=1\n");

  // The next boot that fails to mount never formats again.
  device.mounts = {fail(st::kEspFail)};
  device.format_args.clear();
  const auto again = run(device, line);
  assert(!again && again.error() == st::FlashMountError::AlreadyFormatted);
  assert(device.format_args == std::vector<bool>{false});
  assert(device.saves == 1);
  assert(text(line) == "OS FLASH mount ok=0 err=ESP_FAIL formatted_flag=1\n");
}

void flag_set_refuses_format() {
  FakeDevice device{.mounts = {fail(st::kEspFail)}, .flag = std::uint8_t{1}};
  std::array<char, st::kFlashLineBytes> line{};
  const auto volume = run(device, line);
  assert(!volume && volume.error() == st::FlashMountError::AlreadyFormatted);
  assert(device.format_args == std::vector<bool>{false});
  assert(device.saves == 0);
  assert(text(line) == "OS FLASH mount ok=0 err=ESP_FAIL formatted_flag=1\n");
}

void other_flag_value_is_not_set() {
  FakeDevice device{.mounts = {fail(st::kEspFail), ok(1)},
                    .flag = std::uint8_t{7}};
  std::array<char, st::kFlashLineBytes> line{};
  const auto volume = run(device, line);
  assert(volume && volume->formatted);
  assert((device.format_args == std::vector<bool>{false, true}));
  assert(device.saves == 1);
}

void unreadable_flag_never_formats() {
  FakeDevice device{.mounts = {fail(st::kEspFail)},
                    .flag = std::unexpected(st::FlashFlagError::ReadFailed)};
  std::array<char, st::kFlashLineBytes> line{};
  const auto volume = run(device, line);
  assert(!volume && volume.error() == st::FlashMountError::FlagUnreadable);
  assert(device.format_args == std::vector<bool>{false});
  assert(device.saves == 0);
  assert(text(line) == "OS FLASH ERROR operation=flag-read\n"
                       "OS FLASH mount ok=0 err=ESP_FAIL formatted_flag=1\n");
}

void missing_partition_never_formats() {
  for (const std::int32_t code : {kEspErrNotFound, kEspErrNoMem}) {
    FakeDevice device{.mounts = {fail(code)}};
    std::array<char, st::kFlashLineBytes> line{};
    const auto volume = run(device, line);
    assert(!volume && volume.error() == st::FlashMountError::NotFormattable);
    assert(device.format_args == std::vector<bool>{false});
    assert(device.saves == 0);
    const std::string expected =
        std::string("OS FLASH mount ok=0 err=") +
        (code == kEspErrNotFound ? "ESP_ERR_NOT_FOUND" : "ESP_ERR_NO_MEM") +
        " formatted_flag=0\n";
    assert(text(line) == expected);
  }
}

void failed_format_keeps_flag_clear() {
  FakeDevice device{.mounts = {fail(st::kEspFail), fail(kEspErrNoMem)}};
  std::array<char, st::kFlashLineBytes> line{};
  const auto volume = run(device, line);
  assert(!volume && volume.error() == st::FlashMountError::FormatFailed);
  assert((device.format_args == std::vector<bool>{false, true}));
  assert(device.saves == 0);
  assert(device.flag && *device.flag == 0);
  assert(text(line) ==
         "OS FLASH mount ok=0 err=ESP_ERR_NO_MEM formatted_flag=0\n");
}

void flag_save_failure_keeps_volume() {
  FakeDevice device{.mounts = {fail(st::kEspFail), ok(0)}, .save_fails = true};
  std::array<char, st::kFlashLineBytes> line{};
  const auto volume = run(device, line);
  assert(volume && volume->formatted);
  assert(text(line) == "OS FLASH ERROR operation=flag-save\n"
                       "OS FLASH mount ok=1 label=spiffs total=4063232 "
                       "free=4059136 formatted=1\n");
}

void unknown_space_reports_zero() {
  FakeDevice device{.mounts = {ok(0)},
                    .space = std::unexpected(st::FlashSpaceError::Unavailable)};
  std::array<char, st::kFlashLineBytes> line{};
  const auto volume = run(device, line);
  assert(volume && volume->total_bytes == 0 && volume->free_bytes == 0);
  assert(text(line) ==
         "OS FLASH mount ok=1 label=spiffs total=0 free=0 formatted=0\n");
}

void longest_lines_fit() {
  // Two lines with the widest numbers still end in '\n' inside the buffer.
  FakeDevice device{.mounts = {fail(st::kEspFail), ok(0)},
                    .save_fails = true,
                    .space = st::FlashSpace{.total_bytes = UINT64_MAX,
                                            .free_bytes = UINT64_MAX}};
  std::array<char, st::kFlashLineBytes> line{};
  const auto volume = run(device, line);
  assert(volume);
  const std::string_view written = text(line);
  assert(written.size() < line.size());
  assert(written.ends_with("formatted=1\n"));
}

} // namespace

int main() {
  mounts_without_format();
  formats_once_and_sets_flag();
  flag_set_refuses_format();
  other_flag_value_is_not_set();
  unreadable_flag_never_formats();
  missing_partition_never_formats();
  failed_format_keeps_flag_clear();
  flag_save_failure_keeps_volume();
  unknown_space_reports_zero();
  longest_lines_fit();
  return 0;
}
