// Host checks for firmware/common/src/json_writer.h. Run by
// tools/test_json_writer.py with the golden JavaScript number table.
#include "json_writer.h"

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {
using opensensor::json::Error;
using opensensor::json::Writer;

int failures = 0;

void check(bool ok, const char *what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL %s\n", what);
    ++failures;
  }
}

std::string real_text(double value) {
  std::array<char, opensensor::json::kRealBytes> out{};
  const auto length = opensensor::json::format_real(out, value);
  if (!length)
    return std::string("<") + opensensor::json::error_name(length.error()) +
           ">";
  return {out.data(), *length};
}

// Every line of js_numbers.txt is "<16 hex bits> <String(value)>".
std::size_t golden(const char *path) {
  std::ifstream in(path);
  check(in.good(), "golden table opens");
  std::string line;
  std::size_t count = 0;
  while (std::getline(in, line)) {
    if (line.empty())
      continue;
    const auto space = line.find(' ');
    check(space == 16, "golden line shape");
    const auto bits = std::strtoull(line.substr(0, 16).c_str(), nullptr, 16);
    const auto value = std::bit_cast<double>(static_cast<std::uint64_t>(bits));
    const auto expected = line.substr(space + 1);
    const auto actual = real_text(value);
    if (actual != expected) {
      std::fprintf(stderr, "FAIL number %s want %s got %s\n",
                   line.substr(0, 16).c_str(), expected.c_str(),
                   actual.c_str());
      ++failures;
    }
    ++count;
  }
  return count;
}

void numbers() {
  check(real_text(-0.0) == "0", "-0 writes 0");
  check(real_text(1e21) == "1e+21", "1e21 exponent form");
  check(real_text(1e20) == "100000000000000000000", "1e20 integer form");
  check(real_text(1e-7) == "1e-7", "1e-7 exponent form");
  check(real_text(1e-6) == "0.000001", "1e-6 fixed form");
  check(real_text(-1.5) == "-1.5", "negative fraction");
  check(real_text(std::numeric_limits<double>::quiet_NaN()) ==
            "<json-not-finite>",
        "NaN fails");
  check(real_text(std::numeric_limits<double>::infinity()) ==
            "<json-not-finite>",
        "infinity fails");
  check(real_text(-std::numeric_limits<double>::infinity()) ==
            "<json-not-finite>",
        "negative infinity fails");
  check(real_text(-std::numeric_limits<double>::max()).size() <= 24,
        "longest number fits");
}

// Writes one document that uses every call.
void sample(Writer &json) {
  json.begin();
  json.text("s", "a\"b\\c\n\t\x01\xc3\xa9");
  json.number("u", 18446744073709551615ULL);
  json.integer("i", -9223372036854775807LL - 1);
  json.real("d", 0.1);
  json.null("n");
  json.boolean("t", true);
  json.boolean("f", false);
  json.raw("r", "[1]");
  json.open_array("a");
  json.string_value("x");
  json.unsigned_value(1);
  json.signed_value(-2);
  json.real_value(2.5);
  json.null_value();
  json.bool_value(false);
  json.begin_array();
  json.begin_array();
  json.end_array();
  json.end_array();
  json.begin_object();
  json.text("k", "v");
  json.end_object();
  json.raw_value("{}");
  json.close_array();
  json.open("o");
  json.close();
  json.close();
}

constexpr std::string_view kSample =
    R"({"s":"a\"b\\c\n\t\u0001)"
    "\xc3\xa9"
    R"(","u":18446744073709551615,"i":-9223372036854775808,"d":0.1,)"
    R"("n":null,"t":true,"f":false,"r":[1],"a":["x",1,-2,2.5,null,false,)"
    R"([[]],{"k":"v"},{}],"o":{}})";

void buffer_mode() {
  std::array<char, 512> out{};
  Writer json(out);
  sample(json);
  const auto length = json.finish();
  check(length.has_value() && *length == kSample.size(), "sample length");
  check(length.has_value() &&
            std::string_view(out.data(), *length) == kSample &&
            out[*length] == '\0',
        "sample bytes and NUL");
  // Every smaller buffer fails with Overflow and never writes past its end.
  for (std::size_t size = 0; size <= kSample.size(); ++size) {
    std::vector<char> small(size + 8, '#');
    Writer short_json(std::span<char>(small.data(), size));
    sample(short_json);
    const auto result = short_json.finish();
    check(!result.has_value() && result.error() == Error::Overflow,
          "short buffer overflows");
    for (std::size_t i = size; i < small.size(); ++i)
      check(small[i] == '#', "no write past the buffer");
  }
}

struct Collect {
  std::string text;
  std::size_t calls = 0;
  std::size_t fail_at = static_cast<std::size_t>(-1);
};

bool collect(void *context, std::span<const char> bytes) {
  auto &state = *static_cast<Collect *>(context);
  if (state.calls++ == state.fail_at)
    return false;
  state.text.append(bytes.data(), bytes.size());
  return true;
}

void sink_mode() {
  for (std::size_t chunk = 1; chunk <= 64; ++chunk) {
    std::vector<char> staging(chunk);
    Collect state;
    Writer json(staging, {.write = collect, .context = &state}, 1 << 20);
    sample(json);
    const auto length = json.finish();
    check(length.has_value() && *length == kSample.size() &&
              state.text == kSample,
          "sink output equals buffer output");
  }
  // The limit counts every byte, and the exact size passes.
  {
    std::array<char, 16> staging{};
    Collect state;
    Writer json(staging, {.write = collect, .context = &state}, kSample.size());
    sample(json);
    check(json.finish().has_value(), "limit equal to the size passes");
  }
  {
    std::array<char, 16> staging{};
    Collect state;
    Writer json(staging, {.write = collect, .context = &state},
                kSample.size() - 1);
    sample(json);
    const auto result = json.finish();
    check(!result.has_value() && result.error() == Error::Overflow,
          "limit one short overflows");
  }
  // A failing sink sticks, for the first, a middle and the last chunk.
  for (const std::size_t fail_at : {0UL, 3UL, 100UL}) {
    std::array<char, 8> staging{};
    Collect state;
    state.fail_at = fail_at;
    Writer json(staging, {.write = collect, .context = &state}, 1 << 20);
    sample(json);
    const auto result = json.finish();
    const bool reached = fail_at < (kSample.size() + 7) / 8;
    check(reached ? (!result.has_value() && result.error() == Error::Sink)
                  : result.has_value(),
          "sink failure sticks");
  }
  {
    Collect state;
    Writer json(std::span<char>{}, {.write = collect, .context = &state}, 64);
    json.begin();
    json.close();
    const auto result = json.finish();
    check(!result.has_value() && result.error() == Error::Overflow,
          "empty staging fails");
  }
}

Error structure_error(void (*steps)(Writer &)) {
  std::array<char, 64> out{};
  Writer json(out);
  steps(json);
  const auto result = json.finish();
  return result.has_value() ? Error::Overflow : result.error();
}

void structure() {
  check(structure_error([](Writer &j) {
          j.begin_array();
          j.key("k");
        }) == Error::Structure,
        "key inside an array");
  check(structure_error([](Writer &j) {
          j.begin_object();
          j.unsigned_value(1);
        }) == Error::Structure,
        "value without a key");
  check(structure_error([](Writer &j) {
          j.begin_object();
          j.end_array();
        }) == Error::Structure,
        "mismatched close");
  check(structure_error([](Writer &j) { j.begin_object(); }) ==
            Error::Structure,
        "open container at finish");
  check(structure_error([](Writer &j) {
          j.begin_object();
          j.key("k");
          j.end_object();
        }) == Error::Structure,
        "key without a value");
  check(structure_error([](Writer &j) {
          j.null_value();
          j.null_value();
        }) == Error::Structure,
        "two top level values");
  check(structure_error([](Writer &) {}) == Error::Structure, "empty document");
  check(structure_error([](Writer &j) { j.key("k"); }) == Error::Structure,
        "top level key");
  check(structure_error([](Writer &j) {
          for (int i = 0; i < 33; ++i)
            j.begin_array();
        }) == Error::Structure,
        "depth cap");
  check(structure_error([](Writer &j) {
          j.begin_array();
          j.real_value(std::nan(""));
          j.end_array();
        }) == Error::NotFinite,
        "NaN element");
  {
    std::array<char, 8> out{};
    Writer json(out);
    json.real_value(12.5);
    const auto result = json.finish();
    check(result.has_value() && std::string_view(out.data()) == "12.5",
          "top level scalar");
  }
}
} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s js_numbers.txt\n", argv[0]);
    return 2;
  }
  const auto count = golden(argv[1]);
  check(count > 7000, "golden table size");
  numbers();
  buffer_mode();
  sink_mode();
  structure();
  if (failures) {
    std::fprintf(stderr, "%d failures\n", failures);
    return 1;
  }
  std::printf("json writer: %zu JavaScript numbers matched, buffer, sink and "
              "structure checks passed\n",
              count);
  return 0;
}
