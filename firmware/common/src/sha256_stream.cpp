#include "sha256_stream.h"

#if defined(ESP_PLATFORM)
#include <psa/crypto.h>

#include <new>
#endif

namespace opensensor::hash {

const char *sha256_error_name(Sha256Error error) {
  switch (error) {
  case Sha256Error::Setup:
    return "sha256-setup";
  case Sha256Error::Update:
    return "sha256-update";
  case Sha256Error::Finish:
    return "sha256-finish";
  }
  return "sha256";
}

#if defined(ESP_PLATFORM)

namespace {
static_assert(sizeof(psa_hash_operation_t) <= 256);
static_assert(alignof(psa_hash_operation_t) <= 8);
psa_hash_operation_t &operation(std::array<std::uint8_t, 256> &storage) {
  return *std::launder(
      reinterpret_cast<psa_hash_operation_t *>(storage.data()));
}
} // namespace

// The operation object is initialized once here, so psa_hash_abort() is
// valid on it from now on (psa/crypto.h, psa_hash_abort).
Sha256Stream::Sha256Stream() {
  new (operation_.data()) psa_hash_operation_t(PSA_HASH_OPERATION_INIT);
}

Sha256Stream::~Sha256Stream() {
  psa_hash_abort(&operation(operation_));
  operation(operation_).~psa_hash_operation_t();
}

std::expected<void, Sha256Error> Sha256Stream::begin() {
  abort();
  // psa_crypto_init is safe to call again once it has succeeded.
  if (psa_crypto_init() != PSA_SUCCESS ||
      psa_hash_setup(&operation(operation_), PSA_ALG_SHA_256) != PSA_SUCCESS) {
    psa_hash_abort(&operation(operation_));
    state_ = State::Failed;
    error_ = Sha256Error::Setup;
    return std::unexpected(Sha256Error::Setup);
  }
  state_ = State::Running;
  return {};
}

void Sha256Stream::update(std::span<const std::uint8_t> bytes) {
  if (state_ == State::Failed)
    return;
  if (state_ != State::Running ||
      (!bytes.empty() && psa_hash_update(&operation(operation_), bytes.data(),
                                         bytes.size()) != PSA_SUCCESS)) {
    psa_hash_abort(&operation(operation_));
    state_ = State::Failed;
    error_ = Sha256Error::Update;
  }
}

std::expected<Sha256Digest, Sha256Error> Sha256Stream::finish() {
  if (state_ != State::Running) {
    const auto error = state_ == State::Failed ? error_ : Sha256Error::Update;
    abort();
    return std::unexpected(error);
  }
  Sha256Digest digest{};
  std::size_t length = 0;
  const bool ok = psa_hash_finish(&operation(operation_), digest.data(),
                                  digest.size(), &length) == PSA_SUCCESS &&
                  length == digest.size();
  abort(); // a failed finish leaves the operation to abort
  if (!ok)
    return std::unexpected(Sha256Error::Finish);
  return digest;
}

void Sha256Stream::abort() {
  psa_hash_abort(&operation(operation_));
  state_ = State::Idle;
}

#else

namespace {
constexpr std::array<std::uint32_t, 8> kInitial = {
    0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
constexpr std::array<std::uint32_t, 64> kRound = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
constexpr std::uint32_t rotr(std::uint32_t x, int n) {
  return (x >> n) | (x << (32 - n));
}
} // namespace

Sha256Stream::Sha256Stream() = default;
Sha256Stream::~Sha256Stream() = default;

std::expected<void, Sha256Error> Sha256Stream::begin() {
  words_ = kInitial;
  used_ = 0;
  length_ = 0;
  state_ = State::Running;
  return {};
}

void Sha256Stream::update(std::span<const std::uint8_t> bytes) {
  if (state_ == State::Failed)
    return;
  if (state_ != State::Running) {
    state_ = State::Failed;
    error_ = Sha256Error::Update;
    return;
  }
  for (const std::uint8_t byte : bytes) {
    block_[used_++] = byte;
    if (used_ == block_.size()) {
      compress();
      used_ = 0;
    }
  }
  length_ += bytes.size();
}

std::expected<Sha256Digest, Sha256Error> Sha256Stream::finish() {
  if (state_ != State::Running) {
    const auto error = state_ == State::Failed ? error_ : Sha256Error::Update;
    abort();
    return std::unexpected(error);
  }
  const std::uint64_t bits = length_ * 8;
  block_[used_++] = 0x80;
  if (used_ > 56) {
    while (used_ < block_.size())
      block_[used_++] = 0;
    compress();
    used_ = 0;
  }
  while (used_ < 56)
    block_[used_++] = 0;
  for (int i = 7; i >= 0; --i)
    block_[used_++] = static_cast<std::uint8_t>(bits >> (8 * i));
  compress();
  Sha256Digest digest{};
  for (std::size_t i = 0; i < words_.size(); ++i)
    for (std::size_t j = 0; j < 4; ++j)
      digest[4 * i + j] = static_cast<std::uint8_t>(words_[i] >> (24 - 8 * j));
  abort();
  return digest;
}

void Sha256Stream::abort() {
  state_ = State::Idle;
  used_ = 0;
  length_ = 0;
}

void Sha256Stream::compress() {
  std::array<std::uint32_t, 64> w{};
  for (std::size_t i = 0; i < 16; ++i)
    w[i] = (std::uint32_t{block_[4 * i]} << 24) |
           (std::uint32_t{block_[4 * i + 1]} << 16) |
           (std::uint32_t{block_[4 * i + 2]} << 8) | block_[4 * i + 3];
  for (std::size_t i = 16; i < 64; ++i) {
    const std::uint32_t s0 =
        rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const std::uint32_t s1 =
        rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  auto [a, b, c, d, e, f, g, h] = words_;
  for (std::size_t i = 0; i < 64; ++i) {
    const std::uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) +
                             ((e & f) ^ (~e & g)) + kRound[i] + w[i];
    const std::uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) +
                             ((a & b) ^ (a & c) ^ (b & c));
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  const std::array<std::uint32_t, 8> add = {a, b, c, d, e, f, g, h};
  for (std::size_t i = 0; i < words_.size(); ++i)
    words_[i] += add[i];
}

#endif

} // namespace opensensor::hash
