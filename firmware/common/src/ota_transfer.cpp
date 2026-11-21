#include "ota_transfer.h"

#include "sync_codec.h"

#include <algorithm>

namespace opensensor::ota {
namespace {
using sync::get_u16;
using sync::get_u32;
using sync::put_u16;
using sync::put_u32;

// ble::Error values, kept in step by static_asserts in ota_lan.cpp.
constexpr std::uint8_t kCodeMalformed = 1;
constexpr std::uint8_t kCodeBadHandle = 5;
constexpr std::uint8_t kCodeRange = 6;
constexpr std::uint8_t kCodeBusy = 7;
constexpr std::uint8_t kCodeStorage = 8;
constexpr std::uint8_t kCodeUnknownOp = 11;
constexpr std::uint8_t kCodeImageRejected = 18;

bool sink_complete(const Sink &sink) {
  return sink.begin && sink.write && sink.digest && sink.finish &&
         sink.activate && sink.revert && sink.abort;
}
} // namespace

WireError wire_error(Error error) {
  switch (error) {
  case Error::Malformed:
    return {.code = kCodeMalformed, .detail = "ota"};
  case Error::UnknownOp:
    return {.code = kCodeUnknownOp, .detail = nullptr};
  case Error::NoTarget:
    return {.code = kCodeStorage, .detail = "no-slot"};
  case Error::Busy:
    return {.code = kCodeBusy, .detail = "ota-owner"};
  case Error::RebootPending:
    return {.code = kCodeBusy, .detail = "reboot-pending"};
  case Error::NotActive:
    return {.code = kCodeBadHandle, .detail = "ota-idle"};
  case Error::Size:
    return {.code = kCodeRange, .detail = "size"};
  case Error::Offset:
    return {.code = kCodeRange, .detail = "offset"};
  case Error::Overrun:
    return {.code = kCodeRange, .detail = "overrun"};
  case Error::Header:
    return {.code = kCodeImageRejected, .detail = "header"};
  case Error::Chip:
    return {.code = kCodeImageRejected, .detail = "chip"};
  case Error::Project:
    return {.code = kCodeImageRejected, .detail = "project"};
  case Error::Incomplete:
    return {.code = kCodeRange, .detail = "incomplete"};
  case Error::Digest:
    return {.code = kCodeImageRejected, .detail = "digest"};
  case Error::Flash:
    return {.code = kCodeStorage, .detail = "flash"};
  case Error::Verify:
    return {.code = kCodeImageRejected, .detail = "verify"};
  case Error::Activate:
    return {.code = kCodeStorage, .detail = "boot"};
  }
  return {.code = kCodeMalformed, .detail = "ota"};
}

Progress Transfer::progress() const {
  return {.phase = phase_,
          .next_offset = next_,
          .size = size_,
          .slot_bytes = target_.slot_bytes};
}

void Transfer::reset() {
  phase_ = Phase::Idle;
  owner_ = 0;
  released_ = false;
  size_ = 0;
  next_ = 0;
  declared_ = {};
}

void Transfer::note_progress() const {
  if (sink_.progress)
    sink_.progress(sink_.context);
}

Error Transfer::fail(Error error) {
  if (phase_ == Phase::Receiving) {
    sink_.abort(sink_.context);
    reset();
  }
  return error;
}

bool Transfer::drop_owner(Owner gone) {
  if (phase_ != Phase::Receiving || owner_ != gone)
    return false;
  static_cast<void>(fail(Error::Busy));
  return true;
}

bool Transfer::release_staged(Owner gone) {
  if (phase_ != Phase::Ready || owner_ != gone || released_)
    return false;
  released_ = true;
  return true;
}

std::expected<void, Error>
Transfer::check_header(std::span<const std::uint8_t> first) const {
  if (first.size() < kHeaderBytes || first[0] != kImageMagic ||
      get_u32(first.data() + kAppDescOffset) != kAppDescMagic)
    return std::unexpected(Error::Header);
  if (get_u16(first.data() + kChipIdOffset) != target_.chip_id)
    return std::unexpected(Error::Chip);
  // project_name is NUL padded. It must equal ours exactly, so a prefix such
  // as "opensensor" never matches "opensensor_cores3".
  const auto *name = first.data() + kProjectNameOffset;
  const auto &ours = target_.project_name;
  if (ours.size() >= kProjectNameBytes ||
      !std::equal(ours.begin(), ours.end(), name,
                  [](char a, std::uint8_t b) {
                    return static_cast<std::uint8_t>(a) == b;
                  }) ||
      name[ours.size()] != 0)
    return std::unexpected(Error::Project);
  return {};
}

std::expected<Progress, Error>
Transfer::begin(std::span<const std::uint8_t> body, Owner requester) {
  if (phase_ == Phase::Ready)
    return std::unexpected(Error::RebootPending);
  if (phase_ == Phase::Receiving && owner_ != requester)
    return std::unexpected(Error::Busy);
  // The owner may restart. Close the old handle before the new one opens.
  static_cast<void>(fail(Error::Busy));
  if (body.size() != kBeginBodyBytes)
    return std::unexpected(Error::Malformed);
  if (!target_.slot_bytes || target_.project_name.empty() ||
      !sink_complete(sink_))
    return std::unexpected(Error::NoTarget);
  const std::uint32_t size = get_u32(body.data());
  if (size < kHeaderBytes || size > target_.slot_bytes)
    return std::unexpected(Error::Size);
  if (!sink_.begin(sink_.context, size))
    return std::unexpected(Error::Flash);
  phase_ = Phase::Receiving;
  owner_ = requester;
  released_ = false;
  size_ = size;
  next_ = 0;
  std::copy_n(body.begin() + 4, kDigestBytes, declared_.begin());
  return progress();
}

std::expected<Progress, Error>
Transfer::write(std::span<const std::uint8_t> body) {
  if (body.size() <= 4 || body.size() - 4 > kMaxChunk)
    return std::unexpected(fail(Error::Malformed));
  const std::uint32_t offset = get_u32(body.data());
  const auto data = body.subspan(4);
  if (offset != next_)
    return std::unexpected(fail(Error::Offset));
  if (data.size() > size_ - next_)
    return std::unexpected(fail(Error::Overrun));
  if (next_ == 0) {
    if (const auto header = check_header(data); !header)
      return std::unexpected(fail(header.error()));
  }
  if (!sink_.write(sink_.context, data))
    return std::unexpected(fail(Error::Flash));
  next_ += static_cast<std::uint32_t>(data.size());
  return progress();
}

std::expected<Progress, Error> Transfer::finish() {
  if (next_ != size_)
    return std::unexpected(fail(Error::Incomplete));
  // esp_ota_end first, so every buffered byte is in flash before the read
  // back, and the image has passed ESP-IDF's own checks. Both read the whole
  // image, so progress runs on each side of them.
  note_progress();
  const auto finished = sink_.finish(sink_.context);
  note_progress();
  if (!finished)
    return std::unexpected(fail(Error::Verify));
  const auto digest = sink_.digest(sink_.context, size_);
  note_progress();
  if (!digest)
    return std::unexpected(fail(Error::Flash));
  if (*digest != declared_)
    return std::unexpected(fail(Error::Digest));
  if (!sink_.activate(sink_.context))
    return std::unexpected(fail(Error::Activate));
  phase_ = Phase::Ready;
  return progress();
}

std::expected<Progress, Error> Transfer::cancel() {
  if (phase_ == Phase::Ready) {
    if (!sink_.revert(sink_.context))
      return std::unexpected(Error::Activate);
    reset();
    return progress();
  }
  static_cast<void>(fail(Error::Busy));
  return progress();
}

std::expected<Progress, Error>
Transfer::handle(std::span<const std::uint8_t> request, Owner requester) {
  if (request.empty())
    return std::unexpected(Error::Malformed);
  const std::uint8_t op = request[0];
  const auto body = request.subspan(1);
  if (op == kOpBegin)
    return begin(body, requester);
  if (op != kOpWrite && op != kOpEnd && op != kOpCancel)
    return std::unexpected(Error::UnknownOp);
  // Cancel with nothing staged is a no-op for any session. A staged image is
  // cancelled by the session that staged it, or by any session once that one
  // closed (release_staged), so another app cannot undo a running update.
  if (op == kOpCancel && phase_ == Phase::Idle)
    return cancel();
  if (op == kOpCancel && phase_ == Phase::Ready) {
    if (requester != owner_ && !released_)
      return std::unexpected(Error::Busy);
    return cancel();
  }
  if (phase_ == Phase::Idle)
    return std::unexpected(Error::NotActive);
  if (phase_ == Phase::Ready)
    return op == kOpEnd ? std::expected<Progress, Error>(progress())
                        : std::unexpected(Error::RebootPending);
  if (requester != owner_)
    return std::unexpected(Error::Busy);
  if (op == kOpWrite)
    return write(body);
  if (op == kOpCancel)
    return cancel();
  if (!body.empty())
    return std::unexpected(fail(Error::Malformed));
  return finish();
}

std::size_t encode_state(const Progress &progress,
                         std::span<std::uint8_t, kStateBytes> out) {
  out[0] = kFrameState;
  out[1] = static_cast<std::uint8_t>(progress.phase);
  put_u32(out.data() + 2, progress.next_offset);
  put_u32(out.data() + 6, progress.size);
  put_u32(out.data() + 10, progress.slot_bytes);
  put_u16(out.data() + 14, static_cast<std::uint16_t>(kMaxChunk));
  return kStateBytes;
}

} // namespace opensensor::ota
