#include <sd_transfer_buffer.h>

#include <cassert>
#include <cstdlib>
#include <type_traits>

namespace {
bool reject_allocations = false;
unsigned allocations = 0, releases = 0;
void *active = nullptr;
template <typename T>
concept Releasable = requires(T &buffer) { buffer.release(); };
} // namespace

void *heap_caps_malloc(std::size_t bytes, std::uint32_t caps) {
  ++allocations;
  assert(bytes == 4096);
  assert(caps == (MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
  if (reject_allocations)
    return nullptr;
  assert(!active);
  active = std::malloc(bytes);
  return active;
}

void heap_caps_free(void *data) {
  if (!data)
    return;
  assert(data == active);
  ++releases;
  std::free(data);
  active = nullptr;
}

int main() {
  using Buffer = opensensor::storage::SdTransferBuffer;
  static_assert(!std::is_copy_constructible_v<Buffer>);
  static_assert(!std::is_copy_assignable_v<Buffer>);
  static_assert(Buffer::kBytes == Buffer::kSectors * 512);
  // No board can drop the reservation, so a failed mount keeps it (#168).
  static_assert(!Releasable<Buffer>);
  {
    // Boot without a card. The mount fails but the buffer stays, so a card
    // inserted after radios fragment DMA memory still gets it.
    Buffer buffer;
    assert(buffer.reserve());
    void *reserved = buffer.data();
    reject_allocations = true; // radios now hold the DMA heap
    assert(buffer.reserve() && buffer.data() == reserved);
    assert(!buffer.report_unavailable()); // reserved, nothing to report
    assert(buffer.reserve() && buffer.data() == reserved);
    assert(allocations == 1 && releases == 0);
    reject_allocations = false;
  }
  assert(!active && allocations == 1 && releases == 1);
  {
    // A boot that could not reserve reports it once, retries quietly and
    // takes the buffer when memory allows.
    Buffer buffer;
    reject_allocations = true;
    assert(!buffer.reserve() && !buffer.data());
    assert(buffer.report_unavailable());
    assert(!buffer.reserve());
    assert(!buffer.report_unavailable());
    reject_allocations = false;
    assert(buffer.reserve() && buffer.data());
    assert(!buffer.report_unavailable());
    assert(allocations == 4 && releases == 1);
  }
  assert(!active && allocations == 4 && releases == 2);
  {
    Buffer never_reserved; // ends without freeing anything
  }
  assert(releases == 2);
}
