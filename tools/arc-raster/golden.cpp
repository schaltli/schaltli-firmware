// Runs this repo's own src/project/ArcRaster.cpp against the pixels the
// designer recorded, one case per invocation.
//
// Reads a header line of geometry and colours, then the pixel list, and
// prints "fill track handle rgb" per pixel. run.js feeds it and does the
// diffing - see that file for why this test exists at all.
#include <cstdio>
#include "ArcRaster.h"

// The device's own truncation, the same one ColorScreenRenderer::
// parseHexColor() and the designer's toRgb565() make: bits thrown away, not
// rounded. Matching what the device actually does is the point.
static ArcRaster::Rgb565 fromHex(unsigned int rgb) {
  ArcRaster::Rgb565 c;
  c.r = (uint8_t)(((rgb >> 16) & 0xFF) >> 3);
  c.g = (uint8_t)(((rgb >> 8) & 0xFF) >> 2);
  c.b = (uint8_t)((rgb & 0xFF) >> 3);
  return c;
}

int main(int argc, char** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: golden <case-file> [fastpath 0|1]\n");
    return 2;
  }
  if (argc >= 3 && argv[2][0] == '0') ArcRaster::fastPathEnabled() = false;

  FILE* f = fopen(argv[1], "r");
  if (f == NULL) {
    fprintf(stderr, "cannot open %s\n", argv[1]);
    return 2;
  }

  int size = 0, thickness = 0, inset = 0, ts = 0, tw = 0, fstart = 0, fw = 0, handleAt = -1;
  int startCapFilled = 0, endCapFilled = 0, framed = 0, n = 0;
  unsigned int cTrack = 0, cFill = 0, cHandle = 0, cBg = 0;
  if (fscanf(f, "%d %d %d %d %d %d %d %d %d %d %d %x %x %x %x %d", &size, &thickness, &inset, &ts,
             &tw, &fstart, &fw, &handleAt, &startCapFilled, &endCapFilled, &framed, &cTrack, &cFill,
             &cHandle, &cBg, &n) != 16) {
    fprintf(stderr, "bad header\n");
    return 2;
  }

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

  const ArcRaster::Rgb565 track = fromHex(cTrack);
  const ArcRaster::Rgb565 fill = fromHex(cFill);
  const ArcRaster::Rgb565 handle = fromHex(cHandle);
  const ArcRaster::Rgb565 background = fromHex(cBg);

  for (int i = 0; i < n; i++) {
    int px = 0, py = 0;
    if (fscanf(f, "%d %d", &px, &py) != 2) {
      fprintf(stderr, "short pixel list at %d\n", i);
      return 2;
    }
    const ArcRaster::PixelBands b = ArcRaster::pixelBands(geom, px, py);
    const int covered = b.fill + b.track + b.handle;
    const uint16_t mixed =
        ArcRaster::blendBands(fill, b.fill, track, b.track, handle, b.handle, background,
                              (uint8_t)(ArcRaster::COVERAGE_MAX - covered));
    const ArcRaster::Rgb565 o = ArcRaster::toRgb565(mixed);
    // Bit replication back to 8 bits, which is what the BMP writer does when
    // it hands a snapshot to the HIL comparison. A plain left shift here
    // would put a constant few-LSB bias into every compared pixel.
    const unsigned int r8 = (o.r << 3) | (o.r >> 2);
    const unsigned int g8 = (o.g << 2) | (o.g >> 4);
    const unsigned int b8 = (o.b << 3) | (o.b >> 2);
    printf("%d %d %d %u\n", b.fill, b.track, b.handle, (r8 << 16) | (g8 << 8) | b8);
  }
  fclose(f);
  return 0;
}
