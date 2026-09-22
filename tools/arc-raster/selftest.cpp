// What ArcRaster.h's RowSpan comment and its third short cut claim, checked
// over every pixel of a geometry rather than over the lines the golden
// samples:
//
//   1. the uniform short cut answers exactly what exhaustive sampling does;
//   2. every pixel rowSpan() excludes is a pixel pixelBands() returns empty
//      for - the row's two ends and the hole in the middle.
//
// Both are firmware-only additions with no counterpart in the designer, so
// the golden cannot see them: they decide which pixels are walked and how
// many samples one evaluation stands for, never what a sub-sample becomes.
// This is the only thing that holds them to that.
#include <cstdio>
#include "ArcRaster.h"

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: selftest <case-file>\n");
    return 2;
  }
  FILE* f = fopen(argv[1], "r");
  if (f == NULL) return 2;

  int size = 0, thickness = 0, inset = 0, ts = 0, tw = 0, fstart = 0, fw = 0, handleAt = -1;
  int startCapFilled = 0, endCapFilled = 0, framed = 0, n = 0;
  unsigned int cTrack = 0, cFill = 0, cHandle = 0, cBg = 0;
  if (fscanf(f, "%d %d %d %d %d %d %d %d %d %d %d %x %x %x %x %d", &size, &thickness, &inset, &ts,
             &tw, &fstart, &fw, &handleAt, &startCapFilled, &endCapFilled, &framed, &cTrack, &cFill,
             &cHandle, &cBg, &n) != 16) {
    return 2;
  }
  fclose(f);

  ArcRaster::RingGeometry geom;
  geom.size = size;
  geom.thickness = thickness;
  geom.inset = inset;
  geom.track = ArcRaster::makeSector(ts, tw);
  geom.fill = ArcRaster::makeSector(fstart, fw);
  const ArcRaster::Caps ends = ArcRaster::caps(size, thickness, inset, ts, tw);
  geom.hasStartCap = ends.has;
  geom.hasEndCap = ends.has;
  geom.startCap = ends.startCap;
  geom.endCap = ends.endCap;
  geom.startCapFilled = startCapFilled != 0;
  geom.endCapFilled = endCapFilled != 0;
  if (handleAt >= 0) {
    geom.handle = ArcRaster::handleBand(size, thickness, inset, handleAt, tw);
    geom.hasHandle = true;
  }
  geom.framed = framed != 0;

  long fastDisagree = 0, spanDropsCovered = 0, covered = 0, visited = 0;
  for (int py = 0; py < size; py++) {
    const ArcRaster::RowSpan span = ArcRaster::rowSpan(geom, py);
    for (int px = 0; px < size; px++) {
      ArcRaster::fastPathEnabled() = true;
      const ArcRaster::PixelBands a = ArcRaster::pixelBands(geom, px, py);
      ArcRaster::fastPathEnabled() = false;
      const ArcRaster::PixelBands b = ArcRaster::pixelBands(geom, px, py);
      ArcRaster::fastPathEnabled() = true;
      if (a.fill != b.fill || a.track != b.track || a.handle != b.handle) {
        if (fastDisagree == 0) {
          printf("    fast/slow differ first at (%d,%d): %d/%d/%d vs %d/%d/%d\n", px, py, a.fill,
                 a.track, a.handle, b.fill, b.track, b.handle);
        }
        fastDisagree++;
      }
      const int total = b.fill + b.track + b.handle;
      if (total > 0) covered++;

      const bool inSpan = px >= span.x0 && px <= span.x1;
      const bool inHole = span.holeX1 >= span.holeX0 && px >= span.holeX0 && px <= span.holeX1;
      const bool walked = inSpan && !inHole;
      if (walked) visited++;
      if (!walked && total > 0) {
        if (spanDropsCovered == 0) {
          printf("    rowSpan drops a covered pixel at (%d,%d): %d/%d/%d\n", px, py, b.fill, b.track,
                 b.handle);
        }
        spanDropsCovered++;
      }
    }
  }
  printf("%ld px, %ld covered, %ld walked by rowSpan, fast-vs-slow diffs=%ld, rowSpan drops=%ld\n",
         (long)size * size, covered, visited, fastDisagree, spanDropsCovered);
  return (fastDisagree == 0 && spanDropsCovered == 0) ? 0 : 1;
}
