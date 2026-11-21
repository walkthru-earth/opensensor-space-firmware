#include "os_gnss_reader.h"

#include <debug_log.h>
#include <driver/uart.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <expected>
#include <optional>
#include <string_view>
#include <type_traits>

namespace opensensor::gnss {
namespace {
// Longest SIMCom CGNSSINFO reply is about 110 bytes. Longer lines are dropped.
constexpr std::size_t kLineBytes = 160;
constexpr std::uint32_t kReplyTimeoutMs = 2000;
// AT+CGNSSPWR=1 has a 10 s maximum response time in the AT manual.
constexpr std::uint32_t kPowerOnTimeoutMs = 10000;
// After this many polls without an answer, ask for the engine state again.
constexpr unsigned kSilentPollsBeforeRecheck = 3;

ReaderConfig settings{};
std::atomic<bool> started{false};
portMUX_TYPE fix_mutex = portMUX_INITIALIZER_UNLOCKED;
Fix newest{};
bool have_fix = false;

void store(const std::optional<Fix> &fix) {
  portENTER_CRITICAL(&fix_mutex);
  have_fix = fix.has_value();
  newest = fix ? *fix : Fix{};
  portEXIT_CRITICAL(&fix_mutex);
}

uart_port_t port() { return static_cast<uart_port_t>(settings.uart_port); }

// Keeps raw receiver positions out of a panic core dump (#172). With
// CONFIG_ESP_COREDUMP_CAPTURE_DRAM off, which check_idf_target.py enforces,
// the dump saves each task's TCB and the live part of its stack, from the
// saved stack pointer up, plus the coredump sections. It never saves .bss,
// .data or the heap. So the reply line lives in `line_buffer` in .bss, and
// the UART driver keeps its ring buffer on the heap. The parse, the Reading
// and the H3 conversion run in parse_line and below it. line_fix scrubs that
// dead stack right after each parse, so the next blocking read, whose frames
// reuse those addresses, finds zeros there. The stores go through a volatile
// pointer so they are not dropped, as opensensor::upload::text::secure_zero
// does for secrets. Declare a WipeOnExit right after the object it wipes.
// For a parse result it wipes the contained Reading, an error result holds no
// position.
template <typename T> void wipe_bytes(T &object) {
  static_assert(std::is_trivially_copyable_v<T>);
  volatile auto *bytes = reinterpret_cast<volatile unsigned char *>(&object);
  for (std::size_t i = 0; i < sizeof(T); ++i)
    bytes[i] = 0;
}

template <typename T> class WipeOnExit {
public:
  explicit WipeOnExit(T &object) : object_(object) {}
  ~WipeOnExit() {
    if constexpr (std::is_trivially_copyable_v<T>)
      wipe_bytes(object_);
    else if (object_)
      wipe_bytes(*object_);
  }
  WipeOnExit(const WipeOnExit &) = delete;
  WipeOnExit &operator=(const WipeOnExit &) = delete;

private:
  T &object_;
};

// Bytes scrub_stack zeroes below its caller. The os-gnss scrub entry in
// tools/stack_budget.json fails `stack-budget --check` when the parse_line
// subtree plus a margin no longer fits in the scrub frame.
constexpr std::size_t kScrubBytes = 2048;

// Overwrites the stack below its caller, where parse_line just ran. It is its
// own frame, never a write below the stack pointer, which an interrupt may
// use at any time.
[[gnu::noinline]] void scrub_stack() {
  std::array<unsigned char, kScrubBytes> frame;
  wipe_bytes(frame);
  // Keeps the array, and so the frame, from being optimised away.
  asm volatile("" : : "r"(frame.data()) : "memory");
}

// One reader task owns the line. .bss is not in the core dump.
std::array<char, kLineBytes> line_buffer{};

void send(std::string_view command) {
  uart_flush_input(port());
  uart_write_bytes(port(), command.data(), command.size());
  uart_write_bytes(port(), "\r\n", 2);
}

enum class Line : std::uint8_t { Text, Timeout };

// Reads one CR LF terminated line into `buffer`, without the terminator.
// Overlong lines are skipped whole. Bounded by `deadline_us`.
Line read_line(std::array<char, kLineBytes> &buffer, std::size_t &length,
               std::int64_t deadline_us) {
  length = 0;
  bool overflow = false;
  for (;;) {
    const auto left_us = deadline_us - esp_timer_get_time();
    if (left_us <= 0)
      return Line::Timeout;
    const auto ticks = pdMS_TO_TICKS(left_us / 1000);
    char c = 0;
    if (uart_read_bytes(port(), &c, 1, ticks ? ticks : 1) != 1)
      continue;
    if (c == '\n') {
      if (overflow || length == 0) {
        length = 0;
        overflow = false;
        continue;
      }
      return Line::Text;
    }
    if (c == '\r')
      continue;
    if (length < buffer.size())
      buffer[length++] = c;
    else
      overflow = true;
  }
}

enum class Reply : std::uint8_t { Ok, Error, Timeout };

// Sends `command` and hands every line before OK or ERROR to `on_line`.
template <typename OnLine>
Reply exchange(std::string_view command, std::uint32_t timeout_ms,
               OnLine &&on_line) {
  send(command);
  const auto deadline =
      esp_timer_get_time() + static_cast<std::int64_t>(timeout_ms) * 1000;
  const WipeOnExit wipe_buffer(line_buffer);
  std::size_t length = 0;
  while (read_line(line_buffer, length, deadline) == Line::Text) {
    const std::string_view line(line_buffer.data(), length);
    if (line == "OK")
      return Reply::Ok;
    if (line == "ERROR" || line.starts_with("+CME ERROR"))
      return Reply::Error;
    on_line(line);
    wipe_bytes(line_buffer);
  }
  return Reply::Timeout;
}

// The engine state, or nullopt when the receiver did not answer.
std::optional<bool> query_power(const Dialect &dialect) {
  std::optional<bool> state;
  exchange(dialect.power_query(), kReplyTimeoutMs, [&](std::string_view line) {
    if (const auto on = dialect.parse_power(line))
      state = *on;
  });
  return state;
}

enum class State : std::uint8_t { Unknown, Silent, Off, On, Fix, NoFix };

void report(State &shown, State now) {
  if (shown == now)
    return;
  shown = now;
  constexpr std::array<const char *, 6> kNames = {
      "unknown", "no-reply", "off", "on", "fix", "no-fix"};
  oslog.printf("OS GNSS state=%s uart=%d tx=%d rx=%d\n",
               kNames[static_cast<std::size_t>(now)], settings.uart_port,
               settings.tx_pin, settings.rx_pin);
}

// Parses one receiver line and converts it at once to a res 8 cell. The
// Reading lives only in this frame and the parse below it, and is zeroed on
// every exit. NotThisLine means the line is not a position reply.
[[nodiscard, gnu::noinline]] std::expected<Fix, ParseError>
parse_line(const Dialect &dialect, std::string_view line) {
  auto reading = dialect.parse_fix(line);
  const WipeOnExit wipe_reading(reading);
  if (!reading)
    return std::unexpected(reading.error());
  return to_fix(*reading, esp_timer_get_time());
}

// parse_line, then a scrub of the dead stack it used. Both calls leave from
// this one frame, so the scrub frame lies on the addresses the parse used
// wherever the compiler inlines this function.
[[nodiscard]] std::expected<Fix, ParseError> line_fix(const Dialect &dialect,
                                                      std::string_view line) {
  const auto fix = parse_line(dialect, line);
  scrub_stack();
  return fix;
}

// One AT poll. Converts at once to a res 8 cell and keeps only the cell.
bool poll_fix(const Dialect &dialect, State &shown) {
  bool answered = false;
  std::optional<Fix> fix;
  exchange(dialect.fix_query(), kReplyTimeoutMs, [&](std::string_view line) {
    const auto parsed = line_fix(dialect, line);
    if (!parsed && parsed.error() == ParseError::NotThisLine)
      return;
    answered = true;
    if (parsed)
      fix = *parsed;
  });
  // No CGNSSINFO line, a timeout or ERROR, counts as a silent poll.
  if (!answered)
    return false;
  store(fix);
  report(shown, fix ? State::Fix : State::NoFix);
  return true;
}

// Streaming dialects send sentences unasked. Parse for one poll period.
void stream_fix(const Dialect &dialect, State &shown) {
  const auto deadline =
      esp_timer_get_time() +
      static_cast<std::int64_t>(settings.poll_period_ms) * 1000;
  const WipeOnExit wipe_buffer(line_buffer);
  std::size_t length = 0;
  while (read_line(line_buffer, length, deadline) == Line::Text) {
    const auto parsed =
        line_fix(dialect, std::string_view(line_buffer.data(), length));
    wipe_bytes(line_buffer);
    if (!parsed && parsed.error() == ParseError::NotThisLine)
      continue;
    store(parsed ? std::optional<Fix>(*parsed) : std::nullopt);
    report(shown, parsed ? State::Fix : State::NoFix);
  }
}

void reader_task(void *) {
  const Dialect &dialect = *settings.dialect;
  if (settings.power_on && !settings.power_on(settings.context))
    oslog.println("OS ERROR operation=gnss-power-on");
  State shown = State::Unknown;
  bool on = dialect.power_query().empty();
  unsigned silent = 0;
  for (;;) {
    if (!on) {
      // Ask until the engine reports on. The READY URC is not relied on.
      const auto state = query_power(dialect);
      if (!state) {
        report(shown, State::Silent);
      } else if (*state) {
        on = true;
        silent = 0;
        report(shown, State::On);
        continue;
      } else {
        report(shown, State::Off);
        exchange(dialect.power_on(), kPowerOnTimeoutMs,
                 [](std::string_view) {});
      }
    } else if (dialect.fix_query().empty()) {
      stream_fix(dialect, shown);
      continue;
    } else if (poll_fix(dialect, shown)) {
      silent = 0;
    } else if (++silent >= kSilentPollsBeforeRecheck) {
      store(std::nullopt);
      on = false;
      report(shown, State::Silent);
    }
    vTaskDelay(pdMS_TO_TICKS(settings.poll_period_ms));
  }
}
} // namespace

std::expected<void, StartError> start(const ReaderConfig &config) {
  if (config.dialect == nullptr || config.uart_port < 0 ||
      config.uart_port >= SOC_UART_NUM || config.tx_pin < 0 ||
      config.rx_pin < 0 || config.baud <= 0 || config.poll_period_ms == 0)
    return std::unexpected(StartError::InvalidConfig);
  // Claim the reader first so two concurrent calls cannot both install the
  // driver, and release the claim on any failure so a later call can retry.
  if (started.exchange(true))
    return std::unexpected(StartError::AlreadyStarted);
  settings = config;
  const uart_config_t uart{
      .baud_rate = config.baud,
      .data_bits = UART_DATA_8_BITS,
      .parity = UART_PARITY_DISABLE,
      .stop_bits = UART_STOP_BITS_1,
      .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
      .rx_flow_ctrl_thresh = 0,
      .rx_glitch_filt_thresh = 0,
      .source_clk = UART_SCLK_DEFAULT,
      .flags = {},
  };
  if (uart_param_config(port(), &uart) != ESP_OK ||
      uart_set_pin(port(), config.tx_pin, config.rx_pin, UART_PIN_NO_CHANGE,
                   UART_PIN_NO_CHANGE) != ESP_OK ||
      uart_driver_install(port(), 512, 0, 0, nullptr, 0) != ESP_OK) {
    started = false;
    return std::unexpected(StartError::Uart);
  }
  if (xTaskCreatePinnedToCore(reader_task, "os-gnss", config.reader_stack_bytes,
                              nullptr, config.priority, nullptr,
                              config.core) != pdPASS) {
    uart_driver_delete(port());
    started = false;
    return std::unexpected(StartError::Task);
  }
  return {};
}

bool latest(Fix &fix) {
  portENTER_CRITICAL(&fix_mutex);
  const bool present = have_fix;
  if (present)
    fix = newest;
  portEXIT_CRITICAL(&fix_mutex);
  return present;
}
} // namespace opensensor::gnss
