#pragma once

// Length-prefixed byte ring for one transmit peer
// (docs/shared/ble-sync-protocol.md, "LAN transport"). Each frame is stored as
// its u16 LE length followed by the body, exactly as it goes on the wire, so a
// writer can send straight from peek() and consume() whatever the socket
// accepted. The ring is not thread safe: the owner serialises producers and the
// single consumer. A consumer may send from the peeked span without the lock
// because producers only write free space.

#include <cstddef>
#include <cstdint>

namespace ossync {
enum class EnqueueResult : std::uint8_t {
  Ok,       // the whole frame is queued
  Full,     // not enough free space now, retry after the writer drains
  Closed,   // the peer is gone or closing, never retry
  Rejected, // the frame is empty or over the payload limit, the peer stays
};

class FrameRing {
public:
  static constexpr std::size_t kPrefixBytes = 2;
  static constexpr std::size_t kMaxFrame = 0xffff;

  // Storage is owned by the caller and must outlive the ring.
  void attach(std::uint8_t *storage, std::size_t bytes);
  std::size_t capacity() const { return capacity_; }
  // True when a frame of `length` body bytes can ever fit an empty ring.
  bool fits(std::size_t length) const;

  // All or nothing. Full, Closed and Rejected leave the ring unchanged.
  EnqueueResult try_push(const std::uint8_t *frame, std::size_t length);
  // Contiguous queued bytes from the oldest unsent byte, or 0 when empty.
  std::size_t peek(const std::uint8_t **data) const;
  void consume(std::size_t bytes);

  // Drops every frame that has not started on the wire and keeps the rest of
  // a partly sent frame, so the next push lands on a frame boundary. Returns
  // the number of frames dropped.
  std::size_t drop_unstarted();
  // A frame was partly sent; its remaining bytes must go before anything else.
  bool mid_frame() const;

  std::size_t used() const { return used_; }
  std::size_t free() const { return capacity_ - used_; }
  std::size_t frames() const { return frames_; }
  bool empty() const { return used_ == 0; }
  // Monotonic byte counters; a frame pushed when pushed_total() was N is fully
  // sent once consumed_total() reaches N plus its wire size.
  std::uint64_t pushed_total() const { return pushed_total_; }
  std::uint64_t consumed_total() const { return consumed_total_; }

  // Closed refuses pushes but keeps queued bytes for a final flush.
  void close() { closed_ = true; }
  bool closed() const { return closed_; }
  // Empties and reopens the ring for a new connection.
  void reset();

private:
  std::uint8_t byte_at(std::size_t offset) const;
  void write(const std::uint8_t *data, std::size_t length);

  std::uint8_t *storage_ = nullptr;
  std::size_t capacity_ = 0;
  std::size_t head_ = 0; // oldest unsent byte
  std::size_t used_ = 0;
  std::size_t frames_ = 0;
  // Bytes of the head frame still unsent, 0 when head_ is on a boundary.
  std::size_t head_remaining_ = 0;
  std::uint64_t pushed_total_ = 0, consumed_total_ = 0;
  bool closed_ = false;
};
} // namespace ossync
