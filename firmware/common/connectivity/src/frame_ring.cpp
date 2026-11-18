#include "frame_ring.h"

#include <cstring>

namespace ossync {
void FrameRing::attach(std::uint8_t *storage, std::size_t bytes) {
  storage_ = storage;
  capacity_ = storage ? bytes : 0;
  reset();
}

void FrameRing::reset() {
  // Discarded bytes count as consumed so pushed_total() marks stay reachable.
  consumed_total_ += used_;
  head_ = used_ = frames_ = head_remaining_ = 0;
  closed_ = false;
}

bool FrameRing::fits(std::size_t length) const {
  return length > 0 && length <= kMaxFrame &&
         kPrefixBytes + length <= capacity_;
}

std::uint8_t FrameRing::byte_at(std::size_t offset) const {
  return storage_[(head_ + offset) % capacity_];
}

void FrameRing::write(const std::uint8_t *data, std::size_t length) {
  std::size_t tail = (head_ + used_) % capacity_;
  const std::size_t first =
      length < capacity_ - tail ? length : capacity_ - tail;
  std::memcpy(storage_ + tail, data, first);
  std::memcpy(storage_, data + first, length - first);
  used_ += length;
}

EnqueueResult FrameRing::try_push(const std::uint8_t *frame,
                                  std::size_t length) {
  if (closed_ || !storage_)
    return EnqueueResult::Closed;
  if (length == 0 || length > kMaxFrame)
    return EnqueueResult::Rejected;
  if (kPrefixBytes + length > free())
    return EnqueueResult::Full;
  const std::uint8_t prefix[kPrefixBytes] = {
      static_cast<std::uint8_t>(length & 0xff),
      static_cast<std::uint8_t>((length >> 8) & 0xff)};
  write(prefix, sizeof(prefix));
  write(frame, length);
  ++frames_;
  pushed_total_ += kPrefixBytes + length;
  return EnqueueResult::Ok;
}

std::size_t FrameRing::peek(const std::uint8_t **data) const {
  if (!used_) {
    *data = nullptr;
    return 0;
  }
  *data = storage_ + head_;
  return used_ < capacity_ - head_ ? used_ : capacity_ - head_;
}

void FrameRing::consume(std::size_t bytes) {
  if (bytes > used_)
    bytes = used_;
  while (bytes) {
    if (!head_remaining_)
      head_remaining_ =
          kPrefixBytes + (byte_at(0) | (std::size_t(byte_at(1)) << 8));
    const std::size_t take = bytes < head_remaining_ ? bytes : head_remaining_;
    head_ = (head_ + take) % capacity_;
    used_ -= take;
    consumed_total_ += take;
    head_remaining_ -= take;
    bytes -= take;
    if (!head_remaining_)
      --frames_;
  }
}

bool FrameRing::mid_frame() const { return head_remaining_ != 0; }

std::size_t FrameRing::drop_unstarted() {
  const std::size_t dropped = frames_ - (head_remaining_ ? 1 : 0);
  // Pushed totals stay monotonic: dropped bytes count as never sent, so a
  // slot waiting on them reads as delivered and may queue again.
  consumed_total_ += used_ - head_remaining_;
  used_ = head_remaining_;
  frames_ -= dropped;
  return dropped;
}
} // namespace ossync
