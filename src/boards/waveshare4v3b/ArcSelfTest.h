#pragma once
#include <Arduino.h>
#include "../../project/ArcRaster.h"

// Proves the arc rasterizer's exact short cut against the sampling it
// replaces, pixel by pixel, over geometries the installed project does not
// contain.
//
// Why (2026-09-02): pixelBands() supersamples every pixel sixteen times, and
// on a 360px ring that was 185ms of a 312ms screen render. Only about 8% of
// those pixels straddle an edge; the rest lie wholly inside one band, where
// all sixteen samples agree by construction. The short cut detects exactly
// that case - no sector boundary crossing the pixel, and the whole pixel
// between the two radii - and answers with full coverage without sampling.
//
// "Exact by derivation" is a claim, and this is the check. It runs the same
// pixel both ways and compares all three band counts. A real screen came back
// byte-identical after the change, but that screen has one ring at one angle:
// the interesting cases are a sweep past half a turn (inSector switches from
// intersecting two half-planes to unioning them), a marker band making three
// sectors instead of two, a ring so thin it has no uniform interior, and the
// degenerate full circle. None of those is on the installed project.
//
// Cheap enough to run on demand from the debug endpoint - a few hundred
// thousand comparisons, a second or so - and it needs no fixture, no
// designer and no broker, which is what makes it worth having on the device
// rather than only in a harness.
namespace ArcSelfTest {

struct Case {
  const char *name;
  int32_t size;
  int32_t thickness;
  int32_t startDeg;
  int32_t sweepDeg;
  int32_t markerDeg;   // 0 = no marker band
  int32_t fillPercent;
};

// Chosen for the branches they reach, not for looking like real dials.
static const Case CASES[] = {
    {"full ring", 64, 8, 0, 360, 0, 60},
    {"270 dial", 64, 8, 135, 270, 0, 40},
    {"270 dial + marker", 64, 8, 135, 270, 6, 40},
    {"narrow 30", 64, 8, 10, 30, 0, 50},
    {"wide 200", 64, 8, 20, 200, 0, 50},
    {"wide 350", 64, 8, 5, 350, 3, 50},
    {"thin ring", 64, 2, 0, 300, 0, 70},
    {"thick ring", 64, 30, 0, 300, 0, 70},
    {"thickness = radius", 64, 32, 45, 180, 0, 50},
    {"odd size", 51, 7, 33, 247, 5, 33},
    {"tiny", 16, 3, 0, 270, 0, 50},
    {"empty fill", 64, 8, 0, 270, 0, 0},
    {"full fill", 64, 8, 0, 270, 4, 100},
    {"marker at start", 64, 8, 0, 270, 10, 0},
    {"large", 120, 14, 200, 300, 8, 65},
};

inline ArcRaster::RingGeometry buildGeometry(const Case &c)
{
  const int32_t start64 = c.startDeg * ArcRaster::ANGLE_SCALE;
  const int32_t sweep64 = c.sweepDeg * ArcRaster::ANGLE_SCALE;
  const int32_t filled64 = ArcRaster::sweepForPercent(sweep64, (float)c.fillPercent);

  ArcRaster::RingGeometry geom;
  geom.size = c.size;
  geom.thickness = c.thickness;
  geom.track = ArcRaster::makeSector(start64, sweep64);
  geom.fill = ArcRaster::makeSector(start64, filled64);
  if (c.markerDeg > 0)
  {
    const int32_t at = ArcRaster::sweepForPercent(sweep64, (float)c.fillPercent);
    const int32_t width64 = c.markerDeg * ArcRaster::ANGLE_SCALE;
    geom.marker = ArcRaster::makeSector(start64 + at - width64 / 2, width64);
  }
  else
  {
    geom.marker = ArcRaster::makeSector(0, 0);
  }
  return geom;
}

// Returns a report and sets ok to false on any disagreement. Reports the
// first few mismatching pixels rather than only a count: a bound that is off
// by one shows up as a thin arc of them, and their positions say which
// boundary it was.
inline String run(bool &ok)
{
  String out = "arc raster self-test (exact short cut vs 16x sampling)\n";
  ok = true;
  uint32_t totalPixels = 0, totalMismatch = 0;
  const uint32_t started = millis();

  for (const Case &c : CASES)
  {
    const ArcRaster::RingGeometry geom = buildGeometry(c);
    uint32_t mismatch = 0;
    String firstFew;

    for (int32_t py = 0; py < c.size; py++)
    {
      for (int32_t px = 0; px < c.size; px++)
      {
        ArcRaster::fastPathEnabled() = true;
        const ArcRaster::PixelBands fast = ArcRaster::pixelBands(geom, px, py);
        ArcRaster::fastPathEnabled() = false;
        const ArcRaster::PixelBands slow = ArcRaster::pixelBands(geom, px, py);
        ArcRaster::fastPathEnabled() = true;
        totalPixels++;

        if (fast.fill != slow.fill || fast.track != slow.track || fast.marker != slow.marker)
        {
          mismatch++;
          if (mismatch <= 3)
          {
            firstFew += "      (" + String(px) + "," + String(py) + ") fast " + String(fast.fill) + "/" +
                        String(fast.track) + "/" + String(fast.marker) + " slow " + String(slow.fill) + "/" +
                        String(slow.track) + "/" + String(slow.marker) + "\n";
          }
        }
      }
    }

    totalMismatch += mismatch;
    if (mismatch)
    {
      ok = false;
      out += "  FAIL " + String(c.name) + ": " + String(mismatch) + " pixels differ\n" + firstFew;
    }
  }

  out += "  " + String(sizeof(CASES) / sizeof(CASES[0])) + " geometries, " + String(totalPixels) +
         " pixels compared, " + String(totalMismatch) + " mismatches, " + String(millis() - started) + " ms\n";
  out += ok ? "  PASS - the short cut and the sampling agree everywhere\n"
            : "  FAIL - the short cut is not exact\n";
  return out;
}

} // namespace ArcSelfTest
