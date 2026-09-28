// Runs this repo's gradient and glow arithmetic (ArcRaster.cpp's gradient
// and glow, LevelGlow.h's colours) against what the designer recorded - the
// "glowCases" and "glowColours" of the golden file. run.js feeds it and does
// the diffing.
//
//   glow ring <case-file>    header "size thickness inset start64 sweep64
//                            fromEnd filled64 levels n", then n pixels;
//                            prints "step level" per pixel, and checks over
//                            every pixel of the object that rowSpan() lets
//                            each glowing one through (exit 3 if not).
//   glow colour <file>       lines "fr fg fb tr tg tb step steps alpha ur ug
//                            ub"; prints "gr gg gb mr mg mb" per line.
//
// The glow is built here as ColorScreenRenderer::renderArcLevel builds it -
// the designer's arcGlowFor - which is the one piece this repeats rather
// than calls, because the renderer needs Arduino.h and this must not.
#include <cstdio>
#include <cstring>
#include "ArcRaster.h"
#include "LevelGlow.h"

static ArcRaster::Glow glowFor(int size, int thickness, int inset, int fillStart64, int filled64, int lastStep) {
  ArcRaster::Glow g;
  g.rMid = ArcRaster::midRadius(size, thickness, inset);
  g.half = thickness * ArcRaster::SUBPIXEL_SCALE / 2;
  g.fill = ArcRaster::makeSector(fillStart64, filled64);
  ArcRaster::pointAt(size, thickness, inset, fillStart64, &g.endX[0], &g.endY[0]);
  ArcRaster::pointAt(size, thickness, inset, fillStart64 + filled64, &g.endX[1], &g.endY[1]);
  g.stepFrom = 0;
  g.stepTo = lastStep;
  return g;
}

static int ring(const char* file) {
  FILE* f = fopen(file, "r");
  if (f == NULL) return 2;
  int size = 0, thickness = 0, inset = 0, start64 = 0, sweep64 = 0, fromEnd = 0, filled64 = 0, levels = 0, n = 0;
  if (fscanf(f, "%d %d %d %d %d %d %d %d %d", &size, &thickness, &inset, &start64, &sweep64, &fromEnd, &filled64,
             &levels, &n) != 9) {
    fprintf(stderr, "bad header\n");
    return 2;
  }
  static ArcRaster::Gradient gradient;
  ArcRaster::makeGradient(start64, sweep64, &gradient);
  const int lastStep = ArcRaster::stepOfOffset(filled64 > 1 ? filled64 - 1 : 0, sweep64);
  const int fillStart64 = fromEnd ? start64 + sweep64 - filled64 : start64;
  const ArcRaster::Glow glow = glowFor(size, thickness, inset, fillStart64, filled64, lastStep);
  const int centre = size * ArcRaster::SUBPIXEL_SCALE / 2;

  for (int i = 0; i < n; i++) {
    int px = 0, py = 0;
    if (fscanf(f, "%d %d", &px, &py) != 2) return 2;
    const int cx = px * 8 + 4 - centre, cy = py * 8 + 4 - centre;
    int step = ArcRaster::gradientStep(gradient, cx, cy);
    if (fromEnd) step = ArcRaster::GRADIENT_STEPS - 1 - step;
    const int level = filled64 > 0 ? ArcRaster::glowLevel(glow, cx, cy, levels) : 0;
    printf("%d %d\n", step, level);
  }
  fclose(f);

  // rowSpan() widened by the glow must let every glowing pixel through - the
  // renderer walks only what it lets through, so a pixel it drops is a glow
  // pixel never drawn.
  if (filled64 <= 0) return 0;
  ArcRaster::RingGeometry geom;
  geom.size = size;
  geom.thickness = thickness;
  geom.inset = inset;
  geom.track = ArcRaster::makeSector(start64, sweep64);
  geom.fill = glow.fill;
  const ArcRaster::Caps ends = ArcRaster::caps(size, thickness, inset, start64, sweep64);
  geom.hasStartCap = ends.has;
  geom.hasEndCap = ends.has;
  geom.startCap = ends.startCap;
  geom.endCap = ends.endCap;
  geom.glowReach = levels * ArcRaster::SUBPIXEL_SCALE;
  for (int py = 0; py < size; py++) {
    const ArcRaster::RowSpan span = ArcRaster::rowSpan(geom, py);
    for (int px = 0; px < size; px++) {
      const int cx = px * 8 + 4 - centre, cy = py * 8 + 4 - centre;
      if (ArcRaster::glowLevel(glow, cx, cy, levels) == 0) continue;
      const bool inSpan = px >= span.x0 && px <= span.x1;
      const bool inHole = span.holeX1 >= span.holeX0 && px >= span.holeX0 && px <= span.holeX1;
      if (!inSpan || inHole) {
        fprintf(stderr, "rowSpan drops glowing pixel (%d,%d)\n", px, py);
        return 3;
      }
    }
  }
  return 0;
}

static int colour(const char* file) {
  FILE* f = fopen(file, "r");
  if (f == NULL) return 2;
  int v[12];
  while (fscanf(f, "%d %d %d %d %d %d %d %d %d %d %d %d", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7],
                &v[8], &v[9], &v[10], &v[11]) == 12) {
    ArcRaster::Rgb565 from, to, under;
    from.r = (uint8_t)v[0]; from.g = (uint8_t)v[1]; from.b = (uint8_t)v[2];
    to.r = (uint8_t)v[3]; to.g = (uint8_t)v[4]; to.b = (uint8_t)v[5];
    under.r = (uint8_t)v[9]; under.g = (uint8_t)v[10]; under.b = (uint8_t)v[11];
    const ArcRaster::Rgb565 g = levelglow::gradient565(from, to, v[6], v[7]);
    const ArcRaster::Rgb565 m = levelglow::blend565(under, g, v[8]);
    printf("%d %d %d %d %d %d\n", g.r, g.g, g.b, m.r, m.g, m.b);
  }
  fclose(f);
  return 0;
}

// The step without a division against the step with one, over every pixel of
// rings up to the largest panel and at awkward angles: they must never differ.
static int exhaustive() {
  static ArcRaster::Gradient gradient;
  const int sizes[] = {31, 64, 120, 289, 360, 480};
  const int starts[] = {0, 225 * 64, 12 * 64 + 19, 300 * 64, 90 * 64 + 1};
  const int sweeps[] = {360 * 64, 270 * 64, 233 * 64 + 41, 1 * 64, 180 * 64, 359 * 64 + 63};
  long checked = 0;
  for (int size : sizes) {
    for (int start : starts) {
      for (int sweep : sweeps) {
        ArcRaster::makeGradient(start, sweep, &gradient);
        const int centre = size * 8 / 2;
        for (int py = 0; py < size; py++) {
          for (int px = 0; px < size; px++) {
            const int cx = px * 8 + 4 - centre, cy = py * 8 + 4 - centre;
            const int a = ArcRaster::gradientStep(gradient, cx, cy);
            const int b = ArcRaster::gradientStepByDivision(gradient, cx, cy);
            if (a != b) {
              fprintf(stderr, "size %d start %d sweep %d at (%d,%d): %d without division, %d with\n", size, start,
                      sweep, px, py, a, b);
              return 3;
            }
            checked++;
          }
        }
      }
    }
  }
  printf("%ld points agree\n", checked);
  return 0;
}

int main(int argc, char** argv) {
  if (argc >= 2 && strcmp(argv[1], "exhaustive") == 0) return exhaustive();
  if (argc < 3) {
    fprintf(stderr, "usage: glow ring|colour <file>\n");
    return 2;
  }
  if (strcmp(argv[1], "ring") == 0) return ring(argv[2]);
  if (strcmp(argv[1], "colour") == 0) return colour(argv[2]);
  return 2;
}
