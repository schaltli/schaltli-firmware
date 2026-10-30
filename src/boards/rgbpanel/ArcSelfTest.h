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
// intersecting two half-planes to unioning them), a handle lying across the
// band and standing out of it on both sides, a ring so thin it has no uniform
// interior, and the degenerate full circle. None of those is on the installed
// project.
//
// Since 2026-09-22 there are two more things the short cut has to be blind to,
// and both are represented below: a rounded cap, whose disc reaches past the
// scale's own angles, and a framed track, whose hollow middle the short cut
// would fill with solid track if it stopped asking about the frame's own two
// pixels. A framed dial is not what these boards normally draw - none of them
// is 1-bit - but the branch is compiled here, and a branch nobody exercises is
// a branch that rots.
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
  int32_t handlePercent;  // < 0 = no handle, and then no room reserved for one
  int32_t fillPercent;
  bool framed;            // the track drawn as its own outline
};

// Chosen for the branches they reach, not for looking like real dials.
static const Case CASES[] = {
    {"full ring", 64, 8, 0, 360, -1, 60, false},
    {"270 dial", 64, 8, 135, 270, -1, 40, false},
    {"270 dial + handle", 64, 8, 135, 270, 40, 40, false},
    {"narrow 30", 64, 8, 10, 30, -1, 50, false},
    {"wide 200", 64, 8, 20, 200, -1, 50, false},
    {"wide 350", 64, 8, 5, 350, 20, 50, false},
    {"thin ring", 64, 2, 0, 300, -1, 70, false},
    {"thick ring", 64, 30, 0, 300, -1, 70, false},
    {"thickness = radius", 64, 32, 45, 180, -1, 50, false},
    {"odd size", 51, 7, 33, 247, 66, 33, false},
    {"tiny", 16, 3, 0, 270, -1, 50, false},
    {"empty fill", 64, 8, 0, 270, -1, 0, false},
    {"full fill", 64, 8, 0, 270, 100, 100, false},
    {"handle at start", 64, 8, 0, 270, 0, 0, false},
    {"large", 120, 14, 200, 300, 65, 65, false},
    // The outline: its two long radii stop where the cap begins, and the cap
    // contributes only its rim. The short cut has to see both, and the hollow
    // middle between them, or a framed band comes back solid.
    {"framed dial", 78, 12, 225, 270, 62, 37, true},
    {"framed dial, no handle", 78, 12, 225, 270, -1, 37, true},
    {"framed full ring", 90, 14, 0, 360, 50, 50, true},
    {"framed thin", 61, 3, 30, 200, 80, 45, true},
};

inline ArcRaster::RingGeometry buildGeometry(const Case &c)
{
  const int32_t start64 = c.startDeg * ArcRaster::ANGLE_SCALE;
  const int32_t sweep64 = c.sweepDeg * ArcRaster::ANGLE_SCALE;
  const int32_t filled64 = ArcRaster::sweepForPercent(sweep64, (float)c.fillPercent);
  // Room for the handle, reserved whenever the object can have one at all -
  // the renderer's own arcInsetFor(), which is what puts these geometries on
  // the radii a real dial is drawn at.
  int32_t inset = 0;
  if (c.handlePercent >= 0)
  {
    const int32_t wanted = (c.thickness * 7 + 7) / 8;
    const int32_t room = c.size / 2 - c.thickness - 1;
    inset = wanted < room ? wanted : room;
    if (inset < 0) inset = 0;
  }

  ArcRaster::RingGeometry geom;
  geom.size = c.size;
  geom.thickness = c.thickness;
  geom.inset = inset;
  geom.track = ArcRaster::makeSector(start64, sweep64);
  geom.fill = ArcRaster::makeSector(start64, filled64);
  const ArcRaster::Caps ends = ArcRaster::caps(c.size, c.thickness, inset, start64, sweep64);
  geom.hasStartCap = ends.has;
  geom.hasEndCap = ends.has;
  geom.startCap = ends.startCap;
  geom.endCap = ends.endCap;
  geom.startCapFilled = filled64 > 0;
  geom.endCapFilled = filled64 >= sweep64;
  if (c.handlePercent >= 0)
  {
    const int32_t at = ArcRaster::sweepForPercent(sweep64, (float)c.handlePercent);
    geom.handle = ArcRaster::handleBand(c.size, c.thickness, inset, start64 + at, sweep64);
    geom.hasHandle = true;
  }
  geom.framed = c.framed;
  return geom;
}

// Returns a report and sets ok to false on any disagreement. Reports the
// first few mismatching pixels rather than only a count: a bound that is off
// by one shows up as a thin arc of them, and their positions say which
// boundary it was.
inline String run(bool &ok)
{
  String out = "arc raster self-test (short cut and row spans vs 16x sampling)\n";
  ok = true;
  uint32_t totalPixels = 0, totalMismatch = 0;
  uint32_t totalExcluded = 0, totalLeaked = 0;
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

        if (fast.fill != slow.fill || fast.track != slow.track || fast.body != slow.body || fast.handle != slow.handle)
        {
          mismatch++;
          if (mismatch <= 3)
          {
            firstFew += "      (" + String(px) + "," + String(py) + ") fast " + String(fast.fill) + "/" +
                        String(fast.track) + "/" + String(fast.handle) + " slow " + String(slow.fill) + "/" +
                        String(slow.track) + "/" + String(slow.handle) + "\n";
          }
        }
      }
    }

    // The row spans claim that a pixel they exclude cannot be covered. There
    // is one way to check a claim like that and it is to ask about every
    // excluded pixel, because a bound that is one too tight loses a thin arc
    // of pixels that no screenshot of a single dial would show.
    uint32_t leaked = 0;
    String firstLeaks;

    for (int32_t py = 0; py < c.size; py++)
    {
      const ArcRaster::RowSpan span = ArcRaster::rowSpan(geom, py);
      for (int32_t px = 0; px < c.size; px++)
      {
        const bool inHole = span.holeX1 >= span.holeX0 && px >= span.holeX0 && px <= span.holeX1;
        if (px >= span.x0 && px <= span.x1 && !inHole) continue;
        totalExcluded++;

        const ArcRaster::PixelBands b = ArcRaster::pixelBands(geom, px, py);
        if (b.fill || b.track || b.handle)
        {
          leaked++;
          if (leaked <= 3)
          {
            firstLeaks += "      (" + String(px) + "," + String(py) + ") excluded but covers " +
                          String(b.fill) + "/" + String(b.track) + "/" + String(b.handle) + "\n";
          }
        }
      }
    }

    if (leaked)
    {
      ok = false;
      out += "  FAIL " + String(c.name) + ": " + String(leaked) + " excluded pixels are covered\n" + firstLeaks;
    }

    totalLeaked += leaked;
    totalMismatch += mismatch;
    if (mismatch)
    {
      ok = false;
      out += "  FAIL " + String(c.name) + ": " + String(mismatch) + " pixels differ\n" + firstFew;
    }
  }

  out += "  " + String(sizeof(CASES) / sizeof(CASES[0])) + " geometries, " + String(totalPixels) +
         " pixels compared, " + String(totalMismatch) + " mismatches, " + String(millis() - started) + " ms\n";
  out += "  row spans excluded " + String(totalExcluded) + " pixels, " + String(totalLeaked) + " of them covered\n";
  out += ok ? "  PASS - the short cut agrees with the sampling, and nothing lives outside the spans\n"
            : "  FAIL - the short cut or the spans are not exact\n";
  return out;
}

} // namespace ArcSelfTest
