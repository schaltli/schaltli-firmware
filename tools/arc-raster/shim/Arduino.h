// Just enough Arduino for src/project/ArcRaster.cpp to compile on a desktop
// compiler. The rasterizer is deliberately free of hardware, so this is the
// whole of what it borrows from the core.
#pragma once
#include <stdint.h>
#include <math.h>

#define IRAM_ATTR

#ifndef min
#define min(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef max
#define max(a, b) ((a) > (b) ? (a) : (b))
#endif
#ifndef abs
#define abs(x) ((x) > 0 ? (x) : -(x))
#endif
