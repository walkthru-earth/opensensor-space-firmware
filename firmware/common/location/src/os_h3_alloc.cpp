// H3 allocator hooks for the device build. The board sdkconfig defaults set
// CONFIG_EMBEDDED_H3_LOCATION_ALLOC_PREFIX="os_h3_", so the
// embedded-h3-location component compiles H3 with H3_ALLOC_PREFIX=os_h3_ and
// every H3 malloc, calloc, realloc and free lands here (H3 alloc.h). Only
// cellsToMultiPolygon allocates among the H3 calls the firmware makes, and it
// runs once per finalized file in the storage worker.
//
// All of it goes to PSRAM. Most of H3's buffers for a file of many cells are
// over the 16 KB SPIRAM_MALLOC_ALWAYSINTERNAL default and would go there
// anyway, but each outline loop is a small buffer that plain malloc would put
// in internal RAM, which Wi-Fi and NimBLE need. There is no internal RAM
// fallback. A board without PSRAM gets NULL, H3 returns E_MEMORY_ALLOC and the
// caller reports it.
//
// H3 can ask for zero bytes (the whole globe, cellsToMultiPoly.c), and
// heap_caps_malloc(0) returns NULL on ESP-IDF 6.1, which H3 reads as a failed
// allocation. Every size, both calloc arguments included, goes through the
// library's nonzero_alloc_size, which rounds 0 up to 1.
#include <embedded_h3/location.h>
#include <esp_heap_caps.h>
#include <sdkconfig.h>

#include <cstddef>
#include <string_view>

static_assert(std::string_view{CONFIG_EMBEDDED_H3_LOCATION_ALLOC_PREFIX} ==
                  "os_h3_",
              "Set CONFIG_EMBEDDED_H3_LOCATION_ALLOC_PREFIX=\"os_h3_\" in the "
              "board sdkconfig.defaults, or H3 skips these PSRAM hooks");

namespace {
constexpr unsigned kCaps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
using embedded_h3::location::nonzero_alloc_size;
} // namespace

extern "C" {
void *os_h3_malloc(std::size_t size) {
  return heap_caps_malloc(nonzero_alloc_size(size), kCaps);
}

// heap_caps_calloc checks n * size for overflow and zeroes the block.
void *os_h3_calloc(std::size_t count, std::size_t size) {
  return heap_caps_calloc(nonzero_alloc_size(count), nonzero_alloc_size(size),
                          kCaps);
}

// A NULL pointer allocates. Size 0 is rounded up to 1 like the other hooks,
// so it returns a 1 byte block instead of freeing. H3 v4.5.0 calls realloc
// only to shrink a traced outline loop of 3 or more vertices
// (cellsToMultiPoly.c line 367), never with size 0.
void *os_h3_realloc(void *pointer, std::size_t size) {
  return heap_caps_realloc(pointer, nonzero_alloc_size(size), kCaps);
}

// Accepts NULL.
void os_h3_free(void *pointer) { heap_caps_free(pointer); }
}
