#pragma once

// A chart's shapes as fills (designer lib/chart-shapes.ts,
// tasks/charts-firmware-plan.md F4): every outline in eighths of a pixel,
// its colour and how much of it, in drawing order - what the designer and
// this board fill with the same polygon raster, so they are the same pixels.
// A port, function by function, held to the designer's vectors.

#include <cstdint>
#include <string>
#include <vector>

#include "ChartCore.h"
#include "PolygonRaster.h"

namespace chartshapes {

struct Fill {
  std::vector<PolygonRaster::Point> points;
  std::string colour;  // "#rrggbb"
  int alpha = 256;     // of 256
  bool hasClip = false;
  chartcore::Box clip;
  // Instead of alpha, an alpha by row: fadeAlpha(fade, y) (lib/chart-shapes.ts Fill.fade).
  bool hasFade = false;
  chartcore::Fade fade;
};

// A fading fill's alpha on pixel row y (lib/chart-shapes.ts fadeAlpha).
int fadeAlpha(const chartcore::Fade& f, int y);

// How much of the now colour the line at now is, of 256 (lib/chart-shapes.ts NOW_LINE_ALPHA).
constexpr int NOW_LINE_ALPHA = 90;

struct Look {
  std::string grid;
  bool grey = false;
  // The NowPoint's pulse, its phase 0..1 (lib/chart-shapes.ts ShapeLook.pulse);
  // below 0 none - the point stands still.
  double pulse = -1;
  // The chart's now colour: the line at now and a stack's NowPoint; empty, no line.
  std::string now;
};

// One pulse lasts this long, in ms (lib/chart-shapes.ts PULSE_MS).
constexpr unsigned long PULSE_MS = 1800;

std::vector<Fill> chartShapes(const chartcore::Layout& l, const Look& look);

}  // namespace chartshapes
