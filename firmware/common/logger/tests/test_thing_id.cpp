#include "os_logger_thing_id.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>

// The Thing id load of #225: the stage and the code of every failure, a new
// id only when the key is not found, and a stored value that is never
// rewritten.
namespace {
namespace ti = oslogger::thing_id;

constexpr int kInvalidLength = 0x110c;  // ESP_ERR_NVS_INVALID_LENGTH
constexpr int kTypeMismatch = 0x1103;   // ESP_ERR_NVS_TYPE_MISMATCH
constexpr int kNotInitialized = 0x1101; // ESP_ERR_NVS_NOT_INITIALIZED
constexpr int kNotEnoughSpace = 0x1105; // ESP_ERR_NVS_NOT_ENOUGH_SPACE
constexpr int kReadFault = 0x1109;      // any other read error

struct FakeStore {
  int open_result = ti::kOk;
  int get_result = ti::kOk; // used when nothing is stored or forced
  int set_result = ti::kOk;
  int commit_result = ti::kOk;
  std::optional<std::string> stored;
  bool raw = false; // copy `stored` without a NUL, as no real store does
  int opens = 0, closes = 0, sets = 0, commits = 0;
  std::string committed;

  int open() {
    ++opens;
    return open_result;
  }
  int get(std::span<char, ti::kTextBytes> out, std::size_t &length) {
    if (get_result != ti::kOk)
      return get_result;
    if (!stored)
      return ti::kNotFound;
    if (raw) {
      std::memcpy(out.data(), stored->data(), out.size());
      length = out.size();
      return ti::kOk;
    }
    if (stored->size() + 1 > length)
      return kInvalidLength;
    std::memcpy(out.data(), stored->c_str(), stored->size() + 1);
    length = stored->size() + 1;
    return ti::kOk;
  }
  int set(const char *text) {
    ++sets;
    if (set_result == ti::kOk)
      stored = text;
    return set_result;
  }
  int commit() {
    ++commits;
    if (commit_result == ti::kOk && stored)
      committed = *stored;
    return commit_result;
  }
  void close() { ++closes; }
};

void fill_pattern(std::array<std::uint8_t, 16> &bytes) {
  for (std::size_t i = 0; i < bytes.size(); ++i)
    bytes[i] = static_cast<std::uint8_t>(0xf0 + i);
}

constexpr const char *kGood = "0f1e2d3c-4b5a-4978-8695-a4b3c2d1e0f0";

static_assert(ti::valid("0f1e2d3c-4b5a-4978-8695-a4b3c2d1e0f0"));
static_assert(!ti::valid("0F1E2D3C-4B5A-4978-8695-A4B3C2D1E0F0"));
static_assert(!ti::valid("0f1e2d3c4b5a-4978-8695-a4b3c2d1e0f0-"));
static_assert(!ti::valid("0f1e2d3c-4b5a-4978-8695-a4b3c2d1e0f"));
static_assert(!ti::valid("0f1e2d3c-4b5a-4978-8695-a4b3c2d1e0f00"));
static_assert(!ti::valid("0f1e2d3c-4b5a-4978-8695-a4b3c2d1e0g0"));
static_assert(!ti::valid(""));
static_assert(std::string_view{ti::stage_name(ti::Stage::Open)} == "open");
static_assert(std::string_view{ti::stage_name(ti::Stage::Read)} == "read");
static_assert(std::string_view{ti::stage_name(ti::Stage::Write)} == "write");
static_assert(std::string_view{ti::stage_name(ti::Stage::Format)} == "format");

std::array<char, ti::kTextBytes> fresh_text() {
  std::array<char, ti::kTextBytes> text{};
  text.fill('x');
  return text;
}

void expect_error(FakeStore &store, ti::Stage stage, int code) {
  auto text = fresh_text();
  const auto loaded = ti::load_or_create(store, text, fill_pattern);
  assert(!loaded);
  assert(loaded.error().stage == stage);
  assert(loaded.error().code == code);
  assert(text[0] == '\0'); // no unchecked value leaves the load
}

void test_first_boot_writes_a_v4_id() {
  FakeStore store;
  auto text = fresh_text();
  const auto loaded = ti::load_or_create(store, text, fill_pattern);
  assert(loaded);
  assert(std::string{text.data()} == "f0f1f2f3-f4f5-46f7-b8f9-fafbfcfdfeff");
  assert(ti::valid(text.data()));
  assert(store.sets == 1 && store.commits == 1);
  assert(store.committed == text.data());
  assert(store.opens == 1 && store.closes == 1);
}

void test_stored_id_is_read_and_never_written() {
  FakeStore store;
  store.stored = kGood;
  auto text = fresh_text();
  assert(ti::load_or_create(store, text, fill_pattern));
  assert(std::string{text.data()} == kGood);
  assert(store.sets == 0 && store.commits == 0);
  assert(store.closes == 1);
}

void test_open_failure() {
  FakeStore store;
  store.open_result = kNotInitialized;
  expect_error(store, ti::Stage::Open, kNotInitialized);
  assert(store.closes == 0); // nothing to close after a failed open
  assert(store.sets == 0);
}

void test_read_failures_never_write() {
  for (const int code : {kReadFault, kTypeMismatch, kInvalidLength}) {
    FakeStore store;
    store.stored = kGood;
    store.get_result = code;
    expect_error(store, ti::Stage::Read, code);
    assert(store.sets == 0 && store.commits == 0);
    assert(store.closes == 1);
    assert(*store.stored == kGood);
  }
  // A stored string longer than the buffer is a read error of the store.
  FakeStore longer;
  longer.stored = std::string(kGood) + "0";
  expect_error(longer, ti::Stage::Read, kInvalidLength);
  assert(longer.sets == 0);
}

void test_write_failures() {
  FakeStore set_fails;
  set_fails.set_result = kNotEnoughSpace;
  expect_error(set_fails, ti::Stage::Write, kNotEnoughSpace);
  assert(set_fails.commits == 0);
  assert(set_fails.closes == 1);

  FakeStore commit_fails;
  commit_fails.commit_result = kReadFault;
  expect_error(commit_fails, ti::Stage::Write, kReadFault);
  assert(commit_fails.sets == 1 && commit_fails.commits == 1);
  assert(commit_fails.closes == 1);
}

void test_bad_format_is_kept() {
  for (const char *bad : {"0F1E2D3C-4B5A-4978-8695-A4B3C2D1E0F0", "not-a-uuid",
                          "", "0f1e2d3c-4b5a-4978-8695-a4b3c2d1e0f",
                          "0f1e2d3c_4b5a-4978-8695-a4b3c2"
                          "d1e0f0"}) {
    FakeStore store;
    store.stored = bad;
    expect_error(store, ti::Stage::Format, ti::kOk);
    assert(store.sets == 0 && store.commits == 0);
    assert(*store.stored == bad); // the owner's value stays as it was
  }
  // A value with no NUL inside the buffer is not cut to fit.
  FakeStore raw;
  raw.stored = std::string(kGood) + "0";
  raw.raw = true;
  expect_error(raw, ti::Stage::Format, ti::kOk);
  assert(raw.sets == 0);
}

void test_fault_line() {
  char line[128];
  std::snprintf(line, sizeof(line), ti::kFaultLine,
                ti::stage_name(ti::Stage::Read), kTypeMismatch);
  assert(std::string{line} == "PARQUET ERROR operation=thing-identity "
                              "stage=read error=4355 logging=false\n");
}

} // namespace

int main() {
  test_first_boot_writes_a_v4_id();
  test_stored_id_is_read_and_never_written();
  test_open_failure();
  test_read_failures_never_write();
  test_write_failures();
  test_bad_format_is_kept();
  test_fault_line();
  return 0;
}
