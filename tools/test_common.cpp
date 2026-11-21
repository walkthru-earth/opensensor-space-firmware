#include <numeric_sample.h>
#include <sync_codec.h>
#include <time_sync.h>
#include <utc_clock.h>

#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace {
void sample_contract() {
  struct PreviousLayout {
    std::array<std::int64_t, 77> data{};
    std::array<std::uint8_t, 77> valid{};
  };
  static_assert(sizeof(opensensor::NumericSample<77>) == sizeof(PreviousLayout),
                "Extraction must preserve row stride");
  opensensor::NumericSample<5> row;
  for (const auto validity : row.valid)
    assert(validity == 0);
  row.integer(0, -42);
  row.counter(1, std::numeric_limits<std::int64_t>::max());
  row.number(2, 1.25F);
  row.number(3, std::numeric_limits<float>::quiet_NaN());
  row.number(4, std::numeric_limits<float>::infinity());
  std::int32_t integer = 0;
  float number = 0;
  std::memcpy(&integer, &row.data[0], sizeof(integer));
  std::memcpy(&number, &row.data[2], sizeof(number));
  assert(integer == -42 && number == 1.25F);
  assert(row.data[1] == std::numeric_limits<std::int64_t>::max());
  assert((row.valid == std::array<std::uint8_t, 5>{1, 1, 1, 0, 0}));
  row.number(4, -0.0F);
  std::memcpy(&number, &row.data[4], sizeof(number));
  assert(row.valid[4] == 1 && std::signbit(number));
}

void clock_contract() {
  using namespace opensensor::utc;
  static_assert(days_from_civil(1970, 1, 1) == 0);
  static_assert(days_from_civil(1969, 12, 31) == -1);
  static_assert(days_from_civil(2000, 1, 1) == 10957);
  static_assert(days_from_civil(2000, 3, 1) - days_from_civil(2000, 2, 28) ==
                2);
  static_assert(days_from_civil(2100, 3, 1) - days_from_civil(2100, 2, 28) ==
                1);
  static_assert(days_from_civil(2020, 1, 1) * 86400 == kMinEpochSeconds);
  static_assert(days_from_civil(2100, 1, 1) * 86400 == kMaxEpochSeconds);
  assert(supported_epoch(kMinEpochSeconds));
  assert(supported_epoch(kMaxEpochSeconds));
  assert(!supported_epoch(kMinEpochSeconds - 1));
  assert(!supported_epoch(kMaxEpochSeconds + 1));
  assert(supported_anchor(kMinEpochSeconds, 999999));
  assert(supported_anchor(kMaxEpochSeconds, 0));
  assert(!supported_anchor(kMaxEpochSeconds, 1));
  assert(!supported_anchor(kMinEpochSeconds, 1000000));
  assert(!supported_anchor(std::numeric_limits<std::int64_t>::max(), 0));
  assert(anchor_ns(1800000000LL, 123456) == 1800000000123456000LL);
  const auto anchor = kMinEpochSeconds * 1000000000LL;
  assert(estimate_ns(1000020, 1000000, anchor) == anchor + 20000);
  assert(estimate_ns(999980, 1000000, anchor) == anchor - 20000);
  assert(estimate_us(1000020, 1000000, anchor_ns(1800000000LL, 123456)) ==
         1800000000123476LL);
  assert(ns_to_us(anchor_ns(1800000000LL, 999999)) == 1800000000999999LL);
  assert(starts_new_epoch(None, Host, 0));
  assert(starts_new_epoch(None, Rtc, 0));
  assert(starts_new_epoch(Rtc, Host, 0));
  assert(starts_new_epoch(Rtc, Network, 0));
  for (const auto source : {Host, Network}) {
    for (const auto next : {Host, Network}) {
      assert(!starts_new_epoch(source, next, 0));
      assert(!starts_new_epoch(source, next, kEpochDiscontinuityNs));
      assert(!starts_new_epoch(source, next, -kEpochDiscontinuityNs));
      assert(starts_new_epoch(source, next, kEpochDiscontinuityNs + 1));
      assert(starts_new_epoch(source, next, -kEpochDiscontinuityNs - 1));
      assert(starts_new_epoch(source, next,
                              std::numeric_limits<std::int64_t>::min()));
      assert(starts_new_epoch(source, next,
                              std::numeric_limits<std::int64_t>::max()));
    }
  }
  assert(std::strcmp(source_name(None), "none") == 0);
  assert(std::strcmp(source_name(Host), "host") == 0);
  assert(std::strcmp(source_name(Rtc), "rtc") == 0);
  assert(std::strcmp(source_name(Network), "network") == 0);
  assert(std::strcmp(source_name(99), "none") == 0);

  // #173. No source sets a time before the source floor, the same date as
  // the app's phone floor. The 2020 range stays for decoding stored values.
  static_assert(days_from_civil(2026, 10, 29) * 86400 == kAnchorFloorSeconds);
  static_assert(!accepted_anchor(kAnchorFloorSeconds - 1, 999999));
  static_assert(accepted_anchor(kAnchorFloorSeconds, 0));
  static_assert(!accepted_anchor(kMinEpochSeconds, 0));
  static_assert(supported_anchor(kMinEpochSeconds, 0));
  static_assert(accepted_anchor(kMaxEpochSeconds, 0));
  static_assert(!accepted_anchor(kMaxEpochSeconds, 1));
  static_assert(!accepted_anchor(kAnchorFloorSeconds, 1000000));

  // A move of more than a day against a held anchor is a jump. It is taken
  // but never saved to the RTC. The first anchor of a boot is never a jump.
  constexpr std::int64_t kSecondNs = 1000000000LL;
  constexpr std::int64_t kTenYearsNs = 3653LL * 86400 * kSecondNs;
  for (const auto source : {None, Host, Rtc, Network}) {
    assert(clock_jump(source, kTenYearsNs) == (source != None));
    assert(clock_jump(source, -kTenYearsNs) == (source != None));
    assert(!clock_jump(source, kClockJumpNs));
    assert(!clock_jump(source, -kClockJumpNs));
    assert(clock_jump(source, kClockJumpNs + 1) == (source != None));
    assert(clock_jump(source, std::numeric_limits<std::int64_t>::min()) ==
           (source != None));
    assert(clock_jump(source, std::numeric_limits<std::int64_t>::max()) ==
           (source != None));
  }
  static_assert(kClockJumpNs == 86400 * kSecondNs);
  // A ten year jump starts a new epoch and stays out of the RTC.
  for (const auto previous : {Host, Rtc, Network}) {
    for (const auto next : {Host, Network}) {
      assert(starts_new_epoch(previous, next, kTenYearsNs));
      assert(!saves_to_rtc(previous, next, kTenYearsNs));
      assert(!saves_to_rtc(previous, next, -kTenYearsNs));
      assert(saves_to_rtc(previous, next, 3 * kSecondNs));
      assert(saves_to_rtc(previous, next, -kClockJumpNs));
    }
  }
  // The first host or network anchor is saved. An RTC seed never is.
  assert(saves_to_rtc(None, Host, 0) && saves_to_rtc(None, Network, 0));
  for (const auto previous : {None, Host, Rtc, Network})
    assert(!saves_to_rtc(previous, Rtc, 0));
}

void wire_contract() {
  using namespace opensensor::sync;
  std::uint8_t request_bytes[12]{};
  put_i64(request_bytes, 1800000000LL);
  put_u32(request_bytes + 8, 123456);
  TimeRequest request;
  assert(decode_time_request(request_bytes, 12, request) ==
         TimeRequestResult::Ok);
  assert(request.seconds == 1800000000LL && request.subsecond_us == 123456 &&
         request.precision);
  std::uint8_t acknowledgement[20]{};
  assert(encode_time_ack(acknowledgement, request, 123456789) == 20);
  assert(get_i64(acknowledgement) == 1800000000LL);
  assert(get_i64(acknowledgement + 8) == 123456789);
  assert(get_u32(acknowledgement + 16) == 123456);
  assert(decode_time_request(request_bytes, 8, request) ==
         TimeRequestResult::Ok);
  assert(!request.precision && request.subsecond_us == 0);
  assert(encode_time_ack(acknowledgement, request, 123456789) == 16);
  for (const auto length : {0, 7, 9, 11, 13})
    assert(decode_time_request(request_bytes, length, request) ==
           TimeRequestResult::Malformed);
  assert(decode_time_request(nullptr, 8, request) ==
         TimeRequestResult::Malformed);
  put_u32(request_bytes + 8, 1000000);
  assert(decode_time_request(request_bytes, 12, request) ==
         TimeRequestResult::InvalidEpoch);
  put_i64(request_bytes, std::numeric_limits<std::int64_t>::max());
  assert(decode_time_request(request_bytes, 8, request) ==
         TimeRequestResult::InvalidEpoch);
  // SET_TIME before the source floor answers the same invalid epoch error.
  put_i64(request_bytes, opensensor::utc::kAnchorFloorSeconds - 1);
  assert(decode_time_request(request_bytes, 8, request) ==
         TimeRequestResult::InvalidEpoch);
  put_i64(request_bytes, opensensor::utc::kAnchorFloorSeconds);
  assert(decode_time_request(request_bytes, 8, request) ==
         TimeRequestResult::Ok);
  std::uint8_t bytes[8]{};
  put_u16(bytes, 0xa15b);
  assert(bytes[0] == 0x5b && bytes[1] == 0xa1);
  assert(get_u16(bytes) == 0xa15b);
  put_u32(bytes, 0x80ff015b);
  assert(bytes[0] == 0x5b && bytes[1] == 1 && bytes[2] == 0xff &&
         bytes[3] == 0x80);
  assert(get_u32(bytes) == 0x80ff015b);
  put_i64(bytes, -1);
  for (const auto byte : bytes)
    assert(byte == 0xff);
  assert(get_i64(bytes) == -1);
  put_i64(bytes, std::numeric_limits<std::int64_t>::min());
  assert(bytes[7] == 0x80);
  assert(get_i64(bytes) == std::numeric_limits<std::int64_t>::min());
  put_i64(bytes, std::numeric_limits<std::int64_t>::max());
  assert(get_i64(bytes) == std::numeric_limits<std::int64_t>::max());
  const auto *reference = reinterpret_cast<const std::uint8_t *>("123456789");
  const auto whole = crc_update(0xffffffffU, reference, 9) ^ 0xffffffffU;
  const auto split =
      crc_update(crc_update(0xffffffffU, reference, 4), reference + 4, 5) ^
      0xffffffffU;
  assert(whole == 0xcbf43926U && split == whole);
  assert((crc_update(0xffffffffU, nullptr, 0) ^ 0xffffffffU) == 0);
}

// CRC-32/ISO-HDLC golden vectors shared with the phone
// (docs/shared/ble-sync-protocol.md). crc32_update is the bitwise loop on the
// host and the ROM routine on the device; test_common.py also builds this
// file against a host stand-in of that ROM routine.
std::uint32_t crc_of(const std::uint8_t *data, std::size_t size,
                     std::size_t block) {
  std::uint32_t crc = 0xffffffffU;
  for (std::size_t done = 0; done < size;) {
    const std::size_t part = size - done < block ? size - done : block;
    crc = opensensor::sync::crc32_update(crc, data + done, part);
    done += part;
  }
  return crc ^ 0xffffffffU;
}

void crc_contract() {
  using opensensor::sync::crc_update;
  assert(crc_of(nullptr, 0, 4096) == 0);
  const auto *check = reinterpret_cast<const std::uint8_t *>("123456789");
  assert(crc_of(check, 9, 4096) == 0xcbf43926U);
  assert(crc_of(check, 9, 1) == 0xcbf43926U);
  const char *fox = "The quick brown fox jumps over the lazy dog";
  assert(crc_of(reinterpret_cast<const std::uint8_t *>(fox), std::strlen(fox),
                7) == 0x414fa339U);
  // 1 MiB fixture, the largest finalized file OPEN accepts.
  std::vector<std::uint8_t> fixture(1U << 20);
  for (std::size_t i = 0; i < fixture.size(); ++i)
    fixture[i] =
        static_cast<std::uint8_t>(((i * 31 + 7) & 0xff) ^ ((i >> 12) & 0xff));
  const auto bitwise =
      crc_update(0xffffffffU, fixture.data(), fixture.size()) ^ 0xffffffffU;
  assert(bitwise == 0x391e95d7U);
  for (const std::size_t block : {std::size_t(512), std::size_t(4096),
                                  std::size_t(65537), fixture.size()})
    assert(crc_of(fixture.data(), fixture.size(), block) == bitwise);
}

void archive_contract() {
  using namespace opensensor::sync;
  assert(safe_parquet_name("thing=a/year=2026/part-01.parquet"));
  assert(safe_parquet_name("legacy-parquet/a.parquet"));
  assert(!safe_parquet_name(nullptr));
  for (const auto *invalid :
       {".parquet", "/a.parquet", "../a.parquet", "a/../b.parquet",
        "a//b.parquet", "A.parquet", "a\\b.parquet", "a.parquet.partial",
        "a\nb.parquet"})
    assert(!safe_parquet_name(invalid));
  const std::string limit = std::string(376, 'a') + ".parquet";
  assert(safe_parquet_name(limit.c_str()));
  assert(!safe_parquet_name(("a" + limit).c_str()));
  assert(!safe_parquet_name("a.parquet", 8));
  std::uint8_t head[] = {'P', 'A', 'R', '1'};
  std::uint8_t tail[] = {1, 0, 0, 0, 'P', 'A', 'R', '1'};
  assert(parquet_complete(13, head, tail));
  assert(!parquet_complete(12, head, tail));
  assert(!parquet_complete(13, nullptr, tail));
  assert(!parquet_complete(13, head, nullptr));
  assert(!parquet_complete(13, head, tail, 12));
  assert(parquet_complete(1048576, head, tail));
  assert(!parquet_complete(1048577, head, tail));
  head[0] = 'X';
  assert(!parquet_complete(13, head, tail));
  head[0] = 'P';
  tail[7] = 'X';
  assert(!parquet_complete(13, head, tail));
  tail[7] = '1';
  put_u32(tail, 0);
  assert(!parquet_complete(100, head, tail));
  put_u32(tail, 89);
  assert(!parquet_complete(100, head, tail));
  put_u32(tail, 88);
  assert(parquet_complete(100, head, tail));
}
} // namespace

int main() {
  sample_contract();
  clock_contract();
  wire_contract();
  crc_contract();
  archive_contract();
}
