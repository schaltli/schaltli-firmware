#pragma once

// A std allocator that puts its blocks in PSRAM - for containers that are
// large and read rarely, where the internal heap is what the board runs
// short of. Measured 2026-10-08 on the 4.3B: a navigator with eleven
// entries held 55 ScreenObjects and took 87 KB of internal heap, leaving
// 15 KB; ScreenObject is large (ObjectProperties holds dozens of Strings
// inline), so where it lies is what counts.
//
// Off the ESP32 (the PC's native tests, the host tools) it is plain malloc.

#include <cstddef>
#include <cstdlib>
#include <new>

#if defined(ESP_PLATFORM)
#include <esp_heap_caps.h>
#endif

template <typename T>
struct SpiramAllocator {
  using value_type = T;

  SpiramAllocator() noexcept = default;
  template <typename U>
  SpiramAllocator(const SpiramAllocator<U>&) noexcept {}

  T* allocate(std::size_t n) {
#if defined(ESP_PLATFORM)
    void* p = heap_caps_malloc(n * sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    // No PSRAM (or none left): the internal heap rather than nothing.
    if (p == nullptr) p = std::malloc(n * sizeof(T));
#else
    void* p = std::malloc(n * sizeof(T));
#endif
    if (p == nullptr) throw std::bad_alloc();
    return static_cast<T*>(p);
  }

  void deallocate(T* p, std::size_t) noexcept {
#if defined(ESP_PLATFORM)
    heap_caps_free(p);
#else
    std::free(p);
#endif
  }
};

template <typename T, typename U>
bool operator==(const SpiramAllocator<T>&, const SpiramAllocator<U>&) noexcept {
  return true;
}
template <typename T, typename U>
bool operator!=(const SpiramAllocator<T>&, const SpiramAllocator<U>&) noexcept {
  return false;
}
