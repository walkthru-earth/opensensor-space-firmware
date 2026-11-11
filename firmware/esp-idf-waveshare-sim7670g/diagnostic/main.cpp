#include "../board_io.h"
#include <esp_flash.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <os_console.h>
#include <pms_frame.h>

#ifdef CONFIG_SPIRAM
#error This first diagnostic must not initialize PSRAM before its mode is verified.
#endif

// Waveshare ESP32-S3-SIM7670G-4G V2.0 with the owner's four-wire PMS5003T.
// This diagnostic probes TF capacity without formatting or writing files.
namespace {
using board::kSdClk;
using board::kSdCmd;
using board::kSdData0;
using board::kSensorRx;
using board::kSensorTx;
constexpr std::uint32_t kBaud = 9600;
constexpr std::uint32_t kReportMs = 10000;

plantower::Parser parser{plantower::Model::Pms5003t};
plantower::Frame latest{};
std::uint32_t frames = 0;
std::uint32_t last_frame_ms = 0;
std::uint32_t last_report_ms = 0;
} // namespace

void init_board() {
  vTaskDelay(pdMS_TO_TICKS(500));
  opensensor::console::printf(
      "OS DIAG board=waveshare-sim7670g-v2 sensor=PMS5003T "
      "mode=uart-sd-probe\n");
  std::uint32_t flash_bytes = 0;
  const esp_err_t flash_result = esp_flash_get_size(nullptr, &flash_bytes);
  opensensor::console::printf(
      "OS DIAG chip=%s flash_bytes=%lu flash_error=%d psram_bytes=%lu rx=%d "
      "tx=%d baud=%lu\n",
      "ESP32-S3", static_cast<unsigned long>(flash_bytes), flash_result,
      static_cast<unsigned long>(heap_caps_get_total_size(MALLOC_CAP_SPIRAM)),
      kSensorRx, kSensorTx, static_cast<unsigned long>(kBaud));
  const bool sensor_ready = board::start_sensor();
  const bool mounted = board::mount_card();
  opensensor::console::printf(
      "OS TF mounted=%u size_bytes=%llu sensor_ready=%u "
      "clk=%d cmd=%d d0=%d width=1 format_attempted=false\n",
      mounted, static_cast<unsigned long long>(board::card_bytes()),
      sensor_ready, kSdClk, kSdCmd, kSdData0);
  last_report_ms = static_cast<std::uint32_t>(esp_timer_get_time() / 1000);
}

void poll_board() {
  for (unsigned limit = 0; limit < 256; ++limit) {
    const int byte = board::read_sensor();
    if (byte < 0)
      break;
    plantower::Frame frame;
    if (parser.push(static_cast<std::uint8_t>(byte), frame)) {
      latest = frame;
      ++frames;
      last_frame_ms = static_cast<std::uint32_t>(esp_timer_get_time() / 1000);
    }
  }
  const std::uint32_t now =
      static_cast<std::uint32_t>(esp_timer_get_time() / 1000);
  if (static_cast<std::uint32_t>(now - last_report_ms) >= kReportMs) {
    last_report_ms = now;
    const bool fresh =
        frames && static_cast<std::uint32_t>(now - last_frame_ms) <= 5000;
    const bool warmed = now >= 30000;
    opensensor::console::printf(
        "OS PMS frames=%lu fresh=%u warmed=%u error=%u checksum_errors=%lu "
        "length_errors=%lu age_ms=%lu ",
        static_cast<unsigned long>(frames), fresh, warmed, latest.sensor_error,
        static_cast<unsigned long>(parser.checksum_failures()),
        static_cast<unsigned long>(parser.length_failures()),
        frames ? static_cast<unsigned long>(now - last_frame_ms) : 0UL);
    if (fresh && warmed && latest.sensor_error == 0) {
      opensensor::console::printf(
          "pm1=%u pm25=%u pm10=%u temp_c=%.1f rh_pct=%.1f\n",
          latest.atmospheric_pm1, latest.atmospheric_pm25,
          latest.atmospheric_pm10, latest.temperature_deci_c / 10.0f,
          latest.humidity_deci_percent / 10.0f);
    } else {
      opensensor::console::printf("values=null\n");
    }
  }
  const auto ticks = pdMS_TO_TICKS(10);
  vTaskDelay(ticks ? ticks : 1);
}

namespace {
void board_task(void *) {
  init_board();
  for (;;)
    poll_board();
}
} // namespace

extern "C" void app_main() {
  if (!opensensor::console::begin())
    return;
  if (xTaskCreate(board_task, "os-diagnostic", 12288, nullptr, 1, nullptr) !=
      pdPASS)
    opensensor::console::printf("OS ERROR operation=board-task-create\n");
}
