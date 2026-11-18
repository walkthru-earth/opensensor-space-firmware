#pragma once

#include <esp_heap_caps.h>

#include <cstddef>

namespace opensensor::storage {

// Reserve before starting radios. ESP-IDF borrows this heap-owned buffer from
// sdmmc_host_t and queries its allocated size; a static array is not suitable.
// Its single archive worker serializes card transfers, so one buffer serves
// reads and writes without a second lock. Once reserved it stays until the
// object ends, also when a mount fails. A board that boots without a card can
// then mount one inserted later, when radios have fragmented internal DMA
// memory (#168). There is no release, so no board can drop it by mistake.
class SdTransferBuffer {
public:
  static constexpr std::size_t kSectorBytes = 512;
  static constexpr std::size_t kSectors = 8;
  static constexpr std::size_t kBytes = kSectorBytes * kSectors;

  SdTransferBuffer() = default;
  ~SdTransferBuffer() { heap_caps_free(data_); }
  SdTransferBuffer(const SdTransferBuffer &) = delete;
  SdTransferBuffer &operator=(const SdTransferBuffer &) = delete;

  bool reserve() {
    if (!data_)
      data_ = heap_caps_malloc(kBytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!data_)
      reserve_failed_ = true;
    return data_ != nullptr;
  }

  void *data() const { return data_; }

  // True only the first time it is asked after reserve() failed and while
  // nothing is reserved. A board calls it when a remount fails, so the log
  // names reason dma_buffer_unavailable once and not on every storage retry.
  // A mount that failed before it tried to reserve does not use up the report.
  bool report_unavailable() {
    if (data_ || !reserve_failed_ || reported_)
      return false;
    reported_ = true;
    return true;
  }

private:
  void *data_ = nullptr;
  bool reserve_failed_ = false;
  bool reported_ = false;
};

} // namespace opensensor::storage
