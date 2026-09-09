#pragma once
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_async_memcpy.h>

// Where the swipe frame budget actually goes, measured on the glass rather
// than argued from a datasheet.
//
// Why this exists (2026-09-01): swiping was built, measured at 72ms/frame and
// removed as "the bus is full" - then the factory LVGL demo was flashed back
// and slid roughly 80% of the screen smoothly. So the bus is NOT full, and
// the earlier conclusion generalised from one implementation to the hardware.
//
// A second question landed here later (2026-09-09), and the answer was not
// the one being fished for. A screen render begins by filling the whole
// canvas, which cost 43ms against the 25ms this file already recorded for
// the framebuffer, so the fill looked like 18ms of lazy code. It is not:
//
//   fb_memset      25.3 ms   30.3 MB/s   the driver's framebuffer
//   canvas_memset  43.1 ms   17.8 MB/s   our canvas, same call
//   canvas_write32 43.7 ms   17.6 MB/s   our canvas, 32-bit store loop
//   aligned_psram  43.4 ms   17.7 MB/s   a fresh 64-byte aligned buffer
//
// memset and a hand-written store loop agree to within noise, so nothing is
// to be won in the loop, and a freshly allocated buffer asked for the way
// the LCD driver asks for its own is just as slow, so it is not alignment
// or the allocator either. Every buffer we make writes at 17.7 MB/s and the
// driver's framebuffer writes at 30.3. Why that one is different is not yet
// known - the addresses are printed next to the numbers as the next place
// to start. A rewritten canvas fill was built on the strength of the first
// number, measured, and reverted.
//
// The first run settled the diagnosis:
//
//   fb_memset      25.4 ms   30.2 MB/s   write 768KB into the framebuffer
//   psram_read     27.4 ms   28.1 MB/s   read 768KB out of a canvas
//   fb_from_sram   25.5 ms   30.1 MB/s   internal SRAM -> fb, LVGL's flush
//   fb_from_psram  65.6 ms   11.7 MB/s   canvas -> fb
//   compose        66.1 ms   15.1 fps    one real swipe frame
//   cache line     32 bytes              (Waveshare's own IDF demo sets 64)
//
// Reading alone costs 27ms and writing alone 25ms, but doing both costs 66
// rather than 53: interleaving a read burst and a write burst on the same
// PSRAM interface, 32 bytes at a time, spends the difference turning the bus
// around. LVGL never pays it - it renders cheap widgets into internal SRAM
// and pushes only the result, one direction, 25.5ms, 39fps. Ours reads two
// pre-rendered screens out of PSRAM, because a real project takes 782ms to
// draw and cannot be redrawn per frame.
//
// So the fix has to attack the read side, and there are two candidates that
// need no change to how the firmware is built:
//
//   stage_sram   read PSRAM -> internal SRAM, then SRAM -> fb, in chunks.
//                Same bytes, but each phase runs in one direction, so the
//                turnaround should disappear. Ceiling is 27+25 = 53ms.
//   dma_memcpy   hand the copy to GDMA instead of the CPU. DMA does not go
//                through the data cache at all, so the 32-byte line stops
//                being the unit of transfer, and the CPU is free meanwhile.
//
// The measurement deliberately does NOT flush caches around dma_memcpy. That
// is fine for timing and NOT fine for production: the panel's bounce-buffer
// ISR reads the framebuffer through the cache, so a DMA write behind its back
// can be read stale. If dma_memcpy wins, the real implementation owes an
// esp_cache_msync and a re-measurement with it included.
namespace PanelBenchmark {

inline float mbps(size_t bytes, uint32_t us)
{
  if (us == 0) return 0.0f;
  return (float)bytes / (float)us; // bytes per microsecond == MB/s
}

// Cache line size by measurement, not by config file.
//
// Read one byte every S bytes over a fixed region. The number of lines
// fetched is region/lineSize while S <= lineSize - each line serves several
// accesses - and region/S once S passes it. So the time is flat up to the
// line size and falls beyond: the last flat stride IS the line size.
inline uint32_t cacheLine(uint8_t *region, size_t bytes)
{
  volatile uint32_t sink = 0;
  uint32_t t[4];
  const size_t strides[4] = {16, 32, 64, 128};
  for (int i = 0; i < 4; i++)
  {
    const size_t s = strides[i];
    uint32_t start = micros();
    uint32_t acc = 0;
    for (size_t off = 0; off < bytes; off += s) acc += region[off];
    t[i] = micros() - start;
    sink += acc;
  }
  (void)sink;
  // "Flat" means within 25% of the 16-byte stride. The first stride that is
  // clearly faster ends the flat part, so the line is the stride before it.
  uint32_t line = 16;
  for (int i = 1; i < 4; i++)
  {
    if (t[i] * 4 > t[0] * 3) line = strides[i];
    else break;
  }
  return line;
}

// Bumped by the GDMA completion interrupt. Volatile because the waiting loop
// is the only other reader and nothing else orders the two.
static volatile uint32_t dmaDone = 0;

// Deliberately not IRAM_ATTR: an inline function in a header cannot carry it
// without the Xtensa linker refusing it ("dangerous relocation: l32r: literal
// placed after use"), and it does not need it - the flash cache stays enabled
// throughout, and async_memcpy's interrupt is not installed with
// ESP_INTR_FLAG_IRAM.
inline bool dmaCallback(async_memcpy_handle_t, async_memcpy_event_t *, void *)
{
  dmaDone++;
  return false; // no task woken
}

// Runs everything and returns a plain-text report for /api/debug.
//
// src is a rendered canvas and fb the panel framebuffer, both in PSRAM. fb is
// filled with rubbish by this, so the caller must re-render afterwards.
inline String run(uint16_t *fb, uint16_t *src, int16_t w, int16_t h)
{
  const size_t px = (size_t)w * (size_t)h;
  const size_t bytes = px * 2;
  const int passes = 5;
  String out;

  out += "panel benchmark\n";
  out += "  frame " + String(w) + "x" + String(h) + " = " + String(bytes / 1024) + " KB\n";
  out += "  cpu " + String(ESP.getCpuFreqMHz()) + " MHz, psram " + String(ESP.getPsramSize() / 1024) + " KB\n";

  uint32_t best;

  // 1. write only
  best = UINT32_MAX;
  for (int i = 0; i < passes; i++)
  {
    uint32_t t0 = micros();
    memset(fb, (i & 1) ? 0x11 : 0x22, bytes);
    uint32_t d = micros() - t0;
    if (d < best) best = d;
  }
  out += "  fb_memset      " + String(best / 1000.0f, 1) + " ms  " + String(mbps(bytes, best), 1) + " MB/s\n";

  // 1b. the same write into the canvas rather than the framebuffer, and the
  //     same write done as a store loop instead of memset. A screen render
  //     starts by filling the canvas and that cost 43ms where the
  //     framebuffer takes 25, so one of the two has to be true: either the
  //     canvas is slower memory than the framebuffer, or our loop is slower
  //     than memset. Guessing which had already been wrong twice.
  best = UINT32_MAX;
  for (int i = 0; i < passes; i++)
  {
    uint32_t t0 = micros();
    memset(src, (i & 1) ? 0x11 : 0x22, bytes);
    uint32_t d = micros() - t0;
    if (d < best) best = d;
  }
  out += "  canvas_memset  " + String(best / 1000.0f, 1) + " ms  " + String(mbps(bytes, best), 1) + " MB/s\n";

  best = UINT32_MAX;
  for (int i = 0; i < passes; i++)
  {
    const uint32_t pair = (i & 1) ? 0x11111111u : 0x22222222u;
    uint32_t t0 = micros();
    uint32_t *q = (uint32_t *)src;
    for (size_t n = 0; n < px / 2; n++) q[n] = pair;
    uint32_t d = micros() - t0;
    if (d < best) best = d;
  }
  out += "  canvas_write32 " + String(best / 1000.0f, 1) + " ms  " + String(mbps(bytes, best), 1) + " MB/s\n";

  // 1c. and a third buffer, asked for the way the LCD driver asks for its
  //     framebuffer: 64-byte aligned out of PSRAM. If this lands with the
  //     framebuffer rather than the canvas, the canvas is simply allocated
  //     wrong and the fill is 18ms of nothing.
  {
    uint8_t *probe = (uint8_t *)heap_caps_aligned_alloc(64, bytes, MALLOC_CAP_SPIRAM);
    if (probe == nullptr)
    {
      out += "  aligned_psram  - could not allocate\n";
    }
    else
    {
      best = UINT32_MAX;
      for (int i = 0; i < passes; i++)
      {
        uint32_t t0 = micros();
        memset(probe, (i & 1) ? 0x11 : 0x22, bytes);
        uint32_t d = micros() - t0;
        if (d < best) best = d;
      }
      out += "  aligned_psram  " + String(best / 1000.0f, 1) + " ms  " + String(mbps(bytes, best), 1) +
             " MB/s  (64-byte aligned, fresh)\n";
      out += "  addresses      fb " + String((uint32_t)fb, HEX) + "  canvas " + String((uint32_t)src, HEX) +
             "  probe " + String((uint32_t)probe, HEX) + "\n";
      heap_caps_free(probe);
    }
  }

  // 2. read only
  best = UINT32_MAX;
  volatile uint32_t sink = 0;
  for (int i = 0; i < passes; i++)
  {
    uint32_t t0 = micros();
    uint32_t acc = 0;
    const uint32_t *p = (const uint32_t *)src;
    for (size_t n = 0; n < bytes / 4; n += 4) acc += p[n];
    uint32_t d = micros() - t0;
    sink += acc;
    if (d < best) best = d;
  }
  out += "  psram_read     " + String(best / 1000.0f, 1) + " ms  " + String(mbps(bytes, best), 1) + " MB/s\n";

  // 3. internal SRAM -> framebuffer, which is what an LVGL flush costs. A
  //    16KB block reused: the source stays in cache, so this is a realistic
  //    memcpy that is nonetheless a near-pure PSRAM write.
  const size_t blockBytes = 16 * 1024;
  uint8_t *block = (uint8_t *)heap_caps_malloc(blockBytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (block)
  {
    memset(block, 0x5A, blockBytes);
    best = UINT32_MAX;
    for (int i = 0; i < passes; i++)
    {
      uint32_t t0 = micros();
      for (size_t off = 0; off + blockBytes <= bytes; off += blockBytes)
        memcpy((uint8_t *)fb + off, block, blockBytes);
      uint32_t d = micros() - t0;
      if (d < best) best = d;
    }
    out += "  fb_from_sram   " + String(best / 1000.0f, 1) + " ms  " + String(mbps(bytes, best), 1) + " MB/s  (LVGL's flush)\n";
  }
  else
  {
    out += "  fb_from_sram   SKIPPED - no 16KB internal block free\n";
  }

  // 4. canvas -> framebuffer: read PSRAM and write PSRAM, interleaved.
  best = UINT32_MAX;
  for (int i = 0; i < passes; i++)
  {
    uint32_t t0 = micros();
    memcpy(fb, src, bytes);
    uint32_t d = micros() - t0;
    if (d < best) best = d;
  }
  out += "  fb_from_psram  " + String(best / 1000.0f, 1) + " ms  " + String(mbps(bytes, best), 1) + " MB/s\n";

  // 5. the same bytes, but staged: PSRAM -> SRAM, then SRAM -> fb. Each
  //    phase runs in one direction only. If the turnaround theory holds this
  //    lands near 53ms; if it does not, the theory is wrong.
  if (block)
  {
    best = UINT32_MAX;
    for (int i = 0; i < passes; i++)
    {
      uint32_t t0 = micros();
      for (size_t off = 0; off + blockBytes <= bytes; off += blockBytes)
      {
        memcpy(block, (uint8_t *)src + off, blockBytes);
        memcpy((uint8_t *)fb + off, block, blockBytes);
      }
      uint32_t d = micros() - t0;
      if (d < best) best = d;
    }
    out += "  stage_sram     " + String(best / 1000.0f, 1) + " ms  " + String(mbps(bytes, best), 1) + " MB/s  (via 16KB SRAM)\n";
    heap_caps_free(block);
  }

  // 6. let GDMA do it. No cache involved, so the 32-byte line stops being the
  //    unit of transfer, and the CPU is idle while it runs.
  {
    async_memcpy_config_t cfg = ASYNC_MEMCPY_DEFAULT_CONFIG();
    cfg.backlog = 8;
    async_memcpy_handle_t mcp = nullptr;
    esp_err_t err = esp_async_memcpy_install(&cfg, &mcp);
    if (err != ESP_OK)
    {
      out += "  dma_memcpy     UNAVAILABLE - install failed (" + String(esp_err_to_name(err)) + ")\n";
    }
    else
    {
      const size_t chunk = 64 * 1024;
      const uint32_t chunks = (uint32_t)((bytes + chunk - 1) / chunk);
      best = UINT32_MAX;
      bool failed = false;
      for (int i = 0; i < passes && !failed; i++)
      {
        dmaDone = 0;
        uint32_t t0 = micros();
        for (size_t off = 0; off < bytes; off += chunk)
        {
          size_t n = (off + chunk <= bytes) ? chunk : (bytes - off);
          err = esp_async_memcpy(mcp, (uint8_t *)fb + off, (uint8_t *)src + off, n, dmaCallback, nullptr);
          if (err != ESP_OK) { failed = true; break; }
        }
        // Bounded so a lost interrupt cannot wedge the web request.
        uint32_t guard = 0;
        while (dmaDone < chunks && guard++ < 20000000) { }
        uint32_t d = micros() - t0;
        if (guard >= 20000000) { failed = true; break; }
        if (d < best) best = d;
      }
      if (failed)
        out += "  dma_memcpy     FAILED (" + String(esp_err_to_name(err)) + ") - PSRAM may not be a legal DMA endpoint here\n";
      else
        out += "  dma_memcpy     " + String(best / 1000.0f, 1) + " ms  " + String(mbps(bytes, best), 1) + " MB/s  (GDMA, no cache)\n";
      esp_async_memcpy_uninstall(mcp);
    }
  }

  // 7. a real swipe frame, halfway through the gesture: the left part comes
  //    from one screen and the right from the other. The same source is used
  //    twice - the traffic is what is measured here, not the picture.
  const int16_t offset = w / 2;
  best = UINT32_MAX;
  for (int i = 0; i < passes; i++)
  {
    uint32_t t0 = micros();
    for (int16_t y = 0; y < h; y++)
    {
      uint16_t *dst = fb + (size_t)y * w;
      const uint16_t *rowA = src + (size_t)y * w + offset;
      const uint16_t *rowB = src + (size_t)y * w;
      memcpy(dst, rowA, (size_t)(w - offset) * 2);
      memcpy(dst + (w - offset), rowB, (size_t)offset * 2);
    }
    uint32_t d = micros() - t0;
    if (d < best) best = d;
  }
  out += "  compose        " + String(best / 1000.0f, 1) + " ms  " + String(1000000.0f / best, 1) + " fps  (one swipe frame)\n";

  out += "  cache line     " + String(cacheLine((uint8_t *)src, 256 * 1024)) + " bytes\n";
  (void)sink;
  return out;
}

} // namespace PanelBenchmark
