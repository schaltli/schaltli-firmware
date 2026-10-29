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
};

struct Look {
  std::string grid;
  bool grey = false;
};

std::vector<Fill> chartShapes(const chartcore::Layout& l, const Look& look);

}  // namespace chartshapes
