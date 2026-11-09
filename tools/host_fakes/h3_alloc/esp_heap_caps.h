#pragma once
// Host stand-in for the ESP-IDF heap_caps calls that
// firmware/common/location/src/os_h3_alloc.cpp makes, so h3-polygon-test
// compiles the real device hooks. tools/test_h3_polygon.cpp defines the four
// functions over its counting heap with the ESP-IDF 6.1 semantics at commit
// fff9895c (components/heap/heap_caps_base.c). A zero size returns NULL
// (heap_caps_aligned_alloc_base lines 113-116), calloc checks n * size for
// overflow and then allocates the product, so a zero argument also returns
// NULL (heap_caps_calloc_base), realloc of NULL allocates and realloc to
// size 0 frees and returns NULL (heap_caps_realloc_base), and free accepts
// NULL. It models no capability regions, PSRAM or fragmentation.
#include <cstddef>
#include <cstdint>

// The values in components/heap/include/esp_heap_caps.h.
#define MALLOC_CAP_8BIT (1 << 2)
#define MALLOC_CAP_SPIRAM (1 << 10)

extern "C" {
void *heap_caps_malloc(std::size_t size, std::uint32_t caps);
void *heap_caps_calloc(std::size_t n, std::size_t size, std::uint32_t caps);
void *heap_caps_realloc(void *ptr, std::size_t size, std::uint32_t caps);
void heap_caps_free(void *ptr);
}
