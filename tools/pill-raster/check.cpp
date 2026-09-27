// Holds PillRaster::pixelBandsQuick() to PillRaster::pixelBands(), on a
// laptop - run by tools/pill-raster/run.js.
//
// The quick one exists only for speed (PillRaster.h says why), so the whole
// claim is that it answers what the full sampling answers, count for count,
// on every pixel. Checked here on thousands of random controls: runs of every
// size down to empty and degenerate, both directions, radii from none to
// oversized, overlapping in every order - and on every pixel of their bounds
// with a margin around them, where "outside" has to come out right too.
#include <cstdio>
#include <cstdint>
#include <vector>
#include "PillRaster.h"

using PillRaster::Band;

static uint32_t seed = 20260927;
static int32_t rnd(int32_t lo, int32_t hi) {  // inclusive
  seed = seed * 1103515245u + 12345u;
  return lo + (int32_t)((seed >> 8) % (uint32_t)(hi - lo + 1));
}

int main() {
  long pixels = 0, sampled = 0, failures = 0;
  const int controls = 4000;
  for (int c = 0; c < controls && failures < 20; c++) {
    std::vector<Band> bands;
    const int n = rnd(1, 5);
    for (int i = 0; i < n; i++) {
      Band b;
      b.x = rnd(-6, 40);
      b.y = rnd(-6, 40);
      // Mostly real runs, now and then an empty or a degenerate one.
      b.w = rnd(0, 12) == 0 ? rnd(-4, 0) : rnd(1, 70);
      b.h = rnd(0, 12) == 0 ? rnd(-4, 0) : rnd(1, 70);
      b.rLow = rnd(0, 3) == 0 ? 0 : rnd(0, 45);
      b.rHigh = rnd(0, 3) == 0 ? 0 : rnd(0, 45);
      b.vertical = rnd(0, 1) == 1;
      bands.push_back(b);
    }
    std::vector<uint8_t> full(bands.size()), quick(bands.size());
    for (int32_t py = -10; py < 120; py++) {
      for (int32_t px = -10; px < 120; px++) {
        PillRaster::pixelBands(bands, px, py, full.data());
        PillRaster::pixelBandsQuick(bands, px, py, quick.data());
        pixels++;
        bool partial = false;
        for (size_t b = 0; b < bands.size(); b++) {
          if (full[b] != quick[b]) {
            if (failures < 20) {
              std::printf("FAIL control %d pixel (%d,%d) band %zu: sampled %d, quick %d\n", c, (int)px, (int)py, b,
                          (int)full[b], (int)quick[b]);
            }
            failures++;
          }
          if (full[b] != 0 && full[b] != PillRaster::COVERAGE_MAX) partial = true;
        }
        if (partial) sampled++;
      }
    }
  }
  if (failures) {
    std::printf("\npixelBandsQuick disagrees with pixelBands on %ld pixel(s).\n", failures);
    return 1;
  }
  std::printf("  ok   %d random controls, %ld pixels, %ld of them on an edge\n", controls, pixels, sampled);
  std::printf("\nThe quick coverage agrees with the full sampling, count for count.\n");
  return 0;
}
