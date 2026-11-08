#include "ble_advert.h"

#include <cassert>
#include <cstdio>
#include <string_view>

namespace {
void hex(const std::uint8_t *bytes, std::size_t length) {
  for (std::size_t i = 0; i < length; ++i)
    std::printf("%02x", unsigned(bytes[i]));
  std::putchar('\n');
}

// info with the longest value each identity field takes on a board: a 36
// character thing UUID, 12 hex device, 32 hex boot, 64 hex dict and desc,
// a three digit column count. The schema and firmware come from the board's
// telemetry_contract.h through test_ble_advert.py.
int print_info(const char *schema, const char *firmware, const char *desc) {
  const ble::Identity identity{
      .thing = "ffffffff-ffff-ffff-ffff-ffffffffffff",
      .device = "ffffffffffff",
      .boot = "ffffffffffffffffffffffffffffffff",
      .schema = schema,
      .columns = 999,
      .dictionary_sha256 =
          "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
      .firmware = firmware,
      .descriptor_sha256 = desc,
      .name_prefix = "OS-"};
  char info[ble::kMaxInfo];
  const auto length = ble::wire::build_info(info, identity);
  assert(length && *length < ble::kMaxInfo && info[*length] == '\0');
  std::puts(info);
  return 0;
}

void info_bounds() {
  ble::Identity identity{.thing = "s",
                         .device = "d",
                         .boot = "b",
                         .schema = "x",
                         .columns = 0,
                         .dictionary_sha256 = "a",
                         .firmware = "f",
                         .descriptor_sha256 = "0123456789abcdef0123",
                         .name_prefix = "OS-"};
  char info[ble::kMaxInfo];
  const auto length = ble::wire::build_info(info, identity);
  assert(length && std::string_view(info, *length) ==
                       "{\"proto\":3,\"fw\":\"f\",\"schema\":\"x\",\"cols\":0,"
                       "\"dict\":\"a\",\"desc\":\"0123456789abcdef\","
                       "\"thing\":\"s\",\"dev\":\"d\",\"boot\":\"b\","
                       "\"max_read\":16384}");
  // Exactly fits with its NUL, then one byte short.
  char exact[160];
  const auto fitted =
      ble::wire::build_info(std::span(exact, *length + 1), identity);
  assert(fitted && *fitted == *length);
  const auto short_by_one =
      ble::wire::build_info(std::span(exact, *length), identity);
  assert(!short_by_one &&
         short_by_one.error() == ble::wire::InfoError::Overflow &&
         exact[0] == '\0');
  assert(!ble::wire::build_info(std::span<char>(), identity));
  identity.descriptor_sha256 = "0123456789abcde";
  assert(ble::wire::build_info(info, identity).error() ==
         ble::wire::InfoError::Missing);
  identity.descriptor_sha256 = nullptr;
  assert(ble::wire::build_info(info, identity).error() ==
         ble::wire::InfoError::Missing);
}
} // namespace

int main(int argc, char **argv) {
  if (argc == 4)
    return print_info(argv[1], argv[2], argv[3]);
  info_bounds();
  // Emit actual firmware wire bytes for independent decoding by the shipped
  // Python client. This is host interoperability evidence, not a radio test.
  for (unsigned selector = 1; selector <= 6; ++selector) {
    const auto value = ble::wire::uuid(selector);
    hex(value.data(), value.size());
  }
  for (const auto flags :
       {ble::kAdvNoUtc, ble::kAdvNewFiles, ble::kAdvSd, ble::kAdvLan,
        ble::kAdvFail, ble::kAdvClockRestored, ble::kAdvSetup}) {
    const auto advert = ble::wire::legacy_advert(flags, 0x89abcdefU, 0x7654);
    hex(advert.data(), advert.size());
  }
  for (const auto counter : {0U, 0xffffffffU}) {
    const auto advert = ble::wire::legacy_advert(0, counter, 0xffff);
    hex(advert.data(), advert.size());
  }
  for (const char *name : {"OS-6b40", "12345678901"}) {
    ble::wire::ScanResponse response;
    assert(ble::wire::legacy_scan_response(name, response));
    hex(response.bytes.data(), response.length);
  }
  ble::wire::ScanResponse response;
  assert(!ble::wire::legacy_scan_response("123456789012", response));
  assert(!ble::wire::legacy_scan_response(nullptr, response));
  assert(ble::wire::response_capacity(0) == 0);
  assert(ble::wire::response_capacity(3) == 0);
  assert(ble::wire::response_capacity(23) == 20);
  assert(ble::wire::response_capacity(247) == 244);
  assert(ble::wire::response_capacity(517) == 512);
  assert(ble::wire::response_capacity(65535) == 512);
}
