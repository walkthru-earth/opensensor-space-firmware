#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

// Streaming SHA-256 (FIPS 180-4) with no heap. The device build uses PSA
// Crypto, which ESP-IDF backs with the SHA peripheral. The host build uses a
// plain reference so the host tests can check digests. Same API on both.
namespace opensensor::hash {

inline constexpr std::size_t kSha256Bytes = 32;
using Sha256Digest = std::array<std::uint8_t, kSha256Bytes>;

enum class [[nodiscard]] Sha256Error : std::uint8_t {
  Setup,  // psa_crypto_init or psa_hash_setup failed
  Update, // psa_hash_update failed, or update without begin
  Finish, // psa_hash_finish failed or gave the wrong length
};
[[nodiscard]] const char *sha256_error_name(Sha256Error error);

class Sha256Stream {
public:
  Sha256Stream();
  ~Sha256Stream();
  Sha256Stream(const Sha256Stream &) = delete;
  Sha256Stream &operator=(const Sha256Stream &) = delete;

  // Starts a digest and drops any digest in progress.
  [[nodiscard]] std::expected<void, Sha256Error> begin();
  // The first failure sticks until the next begin(), and finish() reports it.
  void update(std::span<const std::uint8_t> bytes);
  // Ends the digest. Another begin() is needed after it, on any result.
  [[nodiscard]] std::expected<Sha256Digest, Sha256Error> finish();
  // Drops any digest in progress.
  void abort();

private:
  enum class State : std::uint8_t { Idle, Running, Failed };
  State state_ = State::Idle;
  Sha256Error error_ = Sha256Error::Update;
#if defined(ESP_PLATFORM)
  // Opaque psa_hash_operation_t, size and alignment checked in the .cpp.
  alignas(8) std::array<std::uint8_t, 256> operation_{};
#else
  void compress();
  std::array<std::uint32_t, 8> words_{};
  std::array<std::uint8_t, 64> block_{};
  std::size_t used_ = 0;
  std::uint64_t length_ = 0;
#endif
};

} // namespace opensensor::hash
