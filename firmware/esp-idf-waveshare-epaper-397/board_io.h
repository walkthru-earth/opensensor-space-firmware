#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>

// This adapter owns the two I2C buses, the TG28 PMIC, the PCF85063 RTC, the
// QMI8658 accelerometer, the keys and the 4-bit SDMMC card of the Waveshare
// ESP32-S3-ePaper-3.97 (SKU 33810). The SSD1677 panel on SPI3 belongs to the
// display task through opensensor::epd, which switches its supply here. Shared
// services borrow hooks, nothing here is board-neutral.
namespace board {
// Waveshare ESP32-S3-ePaper-3.97 pins, the one copy. board_io.cpp configures
// them and the logger prints them in its boot lines.
//
// SEN63C on the GPIO43/44 header with 10 k pull-ups to 3V3, its own bus.
// The console is USB-Serial/JTAG because these two pins are UART0 by default.
inline constexpr int kSensorSda = 43;
inline constexpr int kSensorScl = 44;
inline constexpr std::uint32_t kSensorI2cHz = 100000;
// Internal bus with 4.7 k pull-ups. TG28 PMIC 0x34, PCF85063 RTC 0x51,
// QMI8658 IMU 0x6B, SHTC3 0x70 and ES8311 0x18. The PMIC, the RTC and the
// IMU accelerometer are used, the SHTC3 and the codec stay in reset state.
inline constexpr int kInternalSda = 41;
inline constexpr int kInternalScl = 42;
inline constexpr std::uint32_t kInternalI2cHz = 100000;
inline constexpr std::uint8_t kPmicAddress = 0x34;
inline constexpr std::uint8_t kRtcAddress = 0x51;
// Schematic U2 ties SDO/SA0 to GND, which selects 0x6B, and the boot scan of
// 2026-11-20 found 0x6b on this bus.
inline constexpr std::uint8_t kImuAddress = 0x6B;
// microSD on SDMMC, 4 bit. Always powered, no card detect.
inline constexpr int kSdClk = 16;
inline constexpr int kSdCmd = 17;
inline constexpr int kSdData0 = 15;
inline constexpr int kSdData1 = 7;
inline constexpr int kSdData2 = 8;
inline constexpr int kSdData3 = 18;
// Keys, active low with the internal pull-ups. Up, Down and Press are the
// three user keys, BOOT is the boot key. Source:
// waveshareteam/ESP32-S3-ePaper-3.97 at
// 1ce1c42421c4690aa3a7eef7354599ce00163f59,
// ESP-IDF/08_ESP32-S3_e-Paper-3.97/components/button_bsp/button_bsp.c, which
// names them Button_Up 4, Button_Function 5, Button_Down 6 and Boot 0, all
// active 0 with pull-ups. The PWR side key sits on the TG28 PMIC, not on a
// GPIO, so it stays with the PMIC. GPIO0 is a strapping pin read only at
// reset, reading it as an input afterwards is safe.
inline constexpr int kKeyUp = 4;
inline constexpr int kKeyPress = 5;
inline constexpr int kKeyDown = 6;
inline constexpr int kKeyBoot = 0;

// Configures the keys as inputs with pull-ups.
bool start_keys();
// In ui::Input order, Up, Down, Press, Menu. BOOT is Menu. There is no Exit
// key, the shared UI needs none, Menu closes the settings popup and cancels
// an edit, and the pages wrap.
inline constexpr std::array<int, 4> kKeys{kKeyUp, kKeyDown, kKeyPress,
                                          kKeyBoot};
// Bit i is set while key kKeys[i] is held. Any task.
std::uint8_t keys_held();

// I2C_NUM_1 at kSensorI2cHz and the SEN63C bound at `address`. Board task
// only.
[[nodiscard]] bool start_sensor_bus(std::uint8_t address);
// Sen6xIo adapters for the bound device. One call is one I2C transaction.
bool sensor_write(void *context, std::uint8_t address,
                  std::span<const std::uint8_t> data);
bool sensor_read(void *context, std::uint8_t address,
                 std::span<std::uint8_t> data);
void sensor_delay_ms(void *context, std::uint32_t milliseconds);
int sensor_last_error(); // esp_err_t of the last transaction

// I2C_NUM_0 with the PMIC and RTC bound. Board task, before any other
// internal bus call. The ESP-IDF master driver serialises the bus, so the
// display task may switch the panel supply while the board task reads.
[[nodiscard]] bool start_internal_bus();

// Probes every 7 bit address on one bus and writes the ones that answer.
// Returns how many answered, which may exceed `found.size()`.
enum class Bus : std::uint8_t { Internal, Sensor };
std::size_t scan(Bus bus, std::span<std::uint8_t> found);

// TG28 PMIC, register compatible with the AXP2101 for what is used here.
enum class [[nodiscard]] PmicError : std::uint8_t { NotStarted, I2c };
[[nodiscard]] const char *to_string(PmicError e);
// Chip id register 0x03.
[[nodiscard]] std::expected<std::uint8_t, PmicError> pmic_chip_id();
// LDO enable register 0x90, bit 0 ALDO1 to bit 3 ALDO4.
[[nodiscard]] std::expected<std::uint8_t, PmicError> pmic_ldo_enables();
// Turns on the VBAT and VBUS ADC channels, leaves the rest as they are.
[[nodiscard]] std::expected<void, PmicError> pmic_start_adc();
// ALDO3 feeds the e-paper panel. On sets 3.3 V first.
[[nodiscard]] std::expected<void, PmicError> panel_power(bool on);
struct PowerStatus {
  bool vbus_good = false;
  bool battery_present = false;
  std::optional<std::uint16_t> battery_mv;
  std::optional<std::uint8_t> battery_percent;
};
[[nodiscard]] std::expected<PowerStatus, PmicError> read_power();

// QMI8658A accelerometer, the gyro stays off. Only the orientation hook
// uses it. A board without an answering IMU keeps running, the hook then
// reports nothing.
enum class [[nodiscard]] ImuError : std::uint8_t {
  NotStarted,
  I2c,
  WrongChip,
  ResetTimeout
};
[[nodiscard]] const char *to_string(ImuError e);
struct ImuInfo {
  std::uint8_t who_am_i = 0;
  std::uint8_t revision = 0;
};
// WHO_AM_I check (0x05), soft reset, then little endian with auto increment,
// +-2 g at the 3 Hz low power rate and the accelerometer alone enabled. Board
// task, after start_internal_bus().
[[nodiscard]] std::expected<ImuInfo, ImuError> start_imu();
// Raw accelerometer counts in the IMU frame, 16384 per g at +-2 g. At rest it
// reads +1 g on the axis that points up.
struct Accel {
  std::int16_t x = 0;
  std::int16_t y = 0;
  std::int16_t z = 0;
};
// Board task. One 6 byte burst from AX_L.
[[nodiscard]] std::expected<Accel, ImuError> read_accel();

// Quarter turns are the clockwise turns of the device from its native
// landscape upright pose, seen from the front, 0 to 3. The UI rotates its
// drawing back by the same number. In panel pixels, x to the right and y
// down, gravity points to +y at 0, +x at 1, -y at 2 and -x at 3.
//
// How the IMU axes sit against the panel is not printed in the schematic and
// the Waveshare demo never maps them to a rotation. This table is the
// provisional guess, IMU x along panel x and IMU y along panel y. The owner
// test in the board README confirms or corrects it from the OS IMU lines.
struct ImuAxis {
  std::uint8_t axis = 0; // 0 x, 1 y, 2 z of the IMU
  std::int8_t sign = 1;
};
struct ImuToPanel {
  ImuAxis x{};
  ImuAxis y{};
};
inline constexpr ImuToPanel kImuToPanel{.x = {.axis = 0, .sign = 1},
                                        .y = {.axis = 1, .sign = 1}};
// Half a g in plane before a pose counts, so a nearly flat device reports
// nothing rather than flicker between turns.
inline constexpr std::int32_t kMinInPlane = 8192;

[[nodiscard]] constexpr std::int32_t imu_axis(Accel a, ImuAxis map) {
  const std::int32_t v = map.axis == 0 ? a.x : map.axis == 1 ? a.y : a.z;
  return map.sign < 0 ? -v : v;
}

// Nothing while the device lies flat (|z| at least the larger in-plane
// axis) or leans less than kMinInPlane.
[[nodiscard]] constexpr std::optional<std::uint8_t> quarter_turn(Accel a) {
  // Gravity is the opposite of what the accelerometer reads.
  const std::int32_t gx = -imu_axis(a, kImuToPanel.x);
  const std::int32_t gy = -imu_axis(a, kImuToPanel.y);
  const std::int32_t ax = gx < 0 ? -gx : gx;
  const std::int32_t ay = gy < 0 ? -gy : gy;
  const std::int32_t z = a.z;
  const std::int32_t az = z < 0 ? -z : z;
  const std::int32_t in_plane = ax > ay ? ax : ay;
  if (in_plane < kMinInPlane || az >= in_plane)
    return std::nullopt;
  if (ay >= ax)
    return static_cast<std::uint8_t>(gy > 0 ? 0 : 2);
  return static_cast<std::uint8_t>(gx > 0 ? 1 : 3);
}
static_assert(quarter_turn({.x = 0, .y = -16384, .z = 0}) == 0);
static_assert(quarter_turn({.x = -16384, .y = 0, .z = 0}) == 1);
static_assert(quarter_turn({.x = 0, .y = 16384, .z = 0}) == 2);
static_assert(quarter_turn({.x = 16384, .y = 0, .z = 0}) == 3);
static_assert(!quarter_turn({.x = 0, .y = 0, .z = 16384}));
static_assert(!quarter_turn({.x = 4000, .y = -4000, .z = 15000}));
static_assert(!quarter_turn({.x = -32768, .y = -32768, .z = -32768}));

// Board task, called every loop. About every 500 ms it reads the
// accelerometer and caches the quarter turn, and returns that read for
// logging. Nothing between reads or when the IMU never started.
std::optional<std::expected<Accel, ImuError>>
poll_orientation(std::int64_t now_us);
// Any task. The last quarter turn, or nothing when the IMU never started, the
// last read failed, the device lies flat or the value is older than 1.5 s.
// The UI applies its own hysteresis and stability window.
[[nodiscard]] std::optional<std::uint8_t> read_orientation();
// Any task. The last accelerometer read poll_orientation cached, under the
// same rules as read_orientation() but kept while the device lies flat. The
// display task feeds it to the shared orientation tracker, so the UI never
// touches the I2C bus.
[[nodiscard]] std::optional<Accel> last_accel();

// PCF85063 RTC as UTC seconds since 1970. False while the oscillator stop
// flag is set, the clock is stopped, in 12 hour mode, or out of range.
bool rtc_read(std::int64_t &seconds, void *context);
// Writes UTC seconds and reads them back. Clears the oscillator stop flag.
bool rtc_write(std::int64_t seconds, void *context);

// Mounts FAT at /sd over 4-bit SDMMC. Never formats.
bool mount_card();
// Storage worker only. Unmount and mount again so a reinserted card or space
// freed elsewhere is seen. Never formats.
bool remount_card();
// Any task. True while a card is mounted.
bool card_mounted();
std::uint32_t cluster_bytes(); // 0 when unknown
std::uint32_t volume_serial(); // FAT volume serial, 0 when unknown
bool storage_free_bytes(std::uint64_t &free_bytes);
// Any task. The last value storage_free_bytes read, 0 before the first.
std::uint64_t cached_free_bytes();
std::uint64_t card_bytes();
std::uint64_t storage_total_bytes();
std::uint64_t storage_used_bytes();
} // namespace board
