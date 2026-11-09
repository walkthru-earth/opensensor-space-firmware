// H3 allocator hooks for the device build. CMakeLists.txt compiles H3 with
// H3_ALLOC_PREFIX=os_h3_, so every H3 malloc, calloc, realloc and free lands
// here (H3 alloc.h). Only cellsToMultiPolygon allocates among the H3 calls
// the firmware makes, and it runs once per finalized file in the storage
// worker.
//
// All of it goes to PSRAM. Most of H3's buffers for a file of many cells are
// over the 16 KB SPIRAM_MALLOC_ALWAYSINTERNAL default and would go there
// anyway, but each outline loop is a small buffer that plain malloc would put
// in internal RAM, which Wi-Fi and NimBLE need. There is no internal RAM
// fallback. A board without PSRAM gets NULL, H3 returns E_MEMORY_ALLOC and the
// caller reports it.
#include <esp_heap_caps.h>

#include <cstddef>

namespace {
constexpr unsigned kCaps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
}

extern "C" {
void *os_h3_malloc(std::size_t size) { return heap_caps_malloc(size, kCaps); }

// heap_caps_calloc checks n * size for overflow and zeroes the block.
void *os_h3_calloc(std::size_t count, std::size_t size) {
  return heap_caps_calloc(count, size, kCaps);
}

// libc realloc semantics, a NULL pointer allocates and size 0 frees.
void *os_h3_realloc(void *pointer, std::size_t size) {
  return heap_caps_realloc(pointer, size, kCaps);
}

// Accepts NULL.
void os_h3_free(void *pointer) { heap_caps_free(pointer); }
}
