// Host checks for firmware/common/src/sha256_stream.h. Run by
// tools/test_sha256.py, which compares the printed digests with hashlib.
#include "sha256_stream.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {
using opensensor::hash::Sha256Digest;
using opensensor::hash::Sha256Error;
using opensensor::hash::Sha256Stream;

int failures = 0;

void check(bool ok, const char *what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL %s\n", what);
    ++failures;
  }
}

std::span<const std::uint8_t> bytes_of(std::string_view text) {
  return {reinterpret_cast<const std::uint8_t *>(text.data()), text.size()};
}

std::string hex(const Sha256Digest &digest) {
  static constexpr std::string_view kHex = "0123456789abcdef";
  std::string out;
  for (const auto byte : digest) {
    out += kHex[byte >> 4];
    out += kHex[byte & 0x0f];
  }
  return out;
}

std::string digest_hex(std::span<const std::uint8_t> data, std::size_t chunk) {
  Sha256Stream sha;
  if (!sha.begin().has_value())
    return "<begin>";
  for (std::size_t at = 0; at < data.size(); at += chunk)
    sha.update(data.subspan(at, std::min(chunk, data.size() - at)));
  const auto digest = sha.finish();
  return digest.has_value() ? hex(*digest) : "<finish>";
}

// FIPS 180-4 examples, from the NIST "SHA256.pdf" example values.
void nist() {
  check(digest_hex(bytes_of("abc"), 3) ==
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        "abc");
  check(digest_hex(bytes_of(""), 1) ==
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
        "empty");
  check(digest_hex(bytes_of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomn"
                            "opnopq"),
                   7) ==
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
        "448 bit message");
  const std::string million(1000000, 'a');
  check(digest_hex(bytes_of(million), 4096) ==
            "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
        "one million a");
}

std::vector<std::uint8_t> pattern(std::size_t length) {
  std::vector<std::uint8_t> data(length);
  std::uint32_t state = 0x12345678;
  for (auto &byte : data) {
    state = state * 1664525U + 1013904223U;
    byte = static_cast<std::uint8_t>(state >> 24);
  }
  return data;
}

// Every chunk size gives the same digest, so the CRC read loop can feed any
// fread length.
void chunking() {
  const auto data = pattern(300);
  const auto whole = digest_hex(data, data.size());
  for (std::size_t chunk = 1; chunk <= data.size(); ++chunk)
    check(digest_hex(data, chunk) == whole, "chunk size does not matter");
  Sha256Stream sha;
  check(sha.begin().has_value(), "begin");
  sha.update(std::span<const std::uint8_t>(data).first(100));
  sha.update({});
  sha.update(std::span<const std::uint8_t>(data).subspan(100));
  const auto digest = sha.finish();
  check(digest.has_value() && hex(*digest) == whole, "empty update is a no-op");
}

void state_rules() {
  Sha256Stream sha;
  sha.update(bytes_of("x"));
  auto digest = sha.finish();
  check(!digest.has_value() && digest.error() == Sha256Error::Update,
        "update before begin fails at finish");
  digest = sha.finish();
  check(!digest.has_value() && digest.error() == Sha256Error::Update,
        "finish twice fails");
  // begin() drops a digest in progress.
  check(sha.begin().has_value(), "begin");
  sha.update(bytes_of("garbage"));
  check(sha.begin().has_value(), "begin again");
  sha.update(bytes_of("abc"));
  digest = sha.finish();
  check(digest.has_value() &&
            hex(*digest) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9c"
                            "b410ff61f20015ad",
        "begin restarts");
  // abort() drops it too, and finish then fails.
  check(sha.begin().has_value(), "begin");
  sha.update(bytes_of("abc"));
  sha.abort();
  digest = sha.finish();
  check(!digest.has_value(), "finish after abort fails");
  check(std::string_view(opensensor::hash::sha256_error_name(
            Sha256Error::Finish)) == "sha256-finish",
        "error name");
}
} // namespace

// Prints "<length> <hex>" for each length around the 64 byte block and the
// 56 byte padding edge, for test_sha256.py to compare with hashlib.
int main() {
  nist();
  chunking();
  state_rules();
  if (failures) {
    std::fprintf(stderr, "%d failures\n", failures);
    return 1;
  }
  for (std::size_t length = 0; length <= 200; ++length) {
    const auto data = pattern(length);
    std::printf("%zu %s\n", length, digest_hex(data, 13).c_str());
  }
  for (const std::size_t length : {4095UL, 4096UL, 4097UL, 65537UL}) {
    const auto data = pattern(length);
    std::printf("%zu %s\n", length, digest_hex(data, 4096).c_str());
  }
  return 0;
}
