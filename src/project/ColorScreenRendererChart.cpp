// A chart on the glass (designer device contract §2.13,
// tasks/charts-firmware-plan.md F4): the core's layout (ChartCore) from the
// history the board holds for the shown screen (ChartFeed), its shapes as
// fills (ChartShapes) filled by PolygonRaster into RGB565 pixels over the
// screen's ground, and its texts as text boxes in the chart's font - what the
// designer draws, pixel for pixel (render-chart.ts).

#include <algorithm>
#include <cmath>

#include "../ChartFeed.h"
#include "ChartShapes.h"
#include "ColorScreenRenderer.h"
#include "EnergyDiagram.h"
#include "PolygonRaster.h"

namespace {

// The chart's pixels, as the designer's ChartPixels: each fill mixed over
// what is there, a pixel no fill touched over the screen's ground, one never
// touched left as the canvas has it. One buffer in PSRAM, grown as needed.
struct ChartPixels {
  int w = 0, h = 0;
  uint16_t ground = 0xFFFF;
  uint16_t* data = nullptr;
  uint8_t* touched = nullptr;
  uint8_t mark = 1;
  size_t capacity = 0;

  bool begin(int width, int height, uint16_t groundColour) {
    w = std::max(0, width);
    h = std::max(0, height);
    ground = groundColour;
    const size_t n = (size_t)w * (size_t)h;
    if (n > capacity) {
      free(data);
      free(touched);
      data = (uint16_t*)ps_malloc(n * sizeof(uint16_t));
      touched = (uint8_t*)ps_malloc(n);
      capacity = data && touched ? n : 0;
      if (capacity) memset(touched, 0, capacity);
      mark = 0;
    }
    if (capacity < n) return false;
    if (++mark == 0) {
      memset(touched, 0, capacity);
      mark = 1;
    }
    return true;
  }

  void fill(const chartshapes::Fill& f, uint16_t colour) {
    int cx0 = 0, cy0 = 0, cx1 = w, cy1 = h;
    if (f.hasClip) {
      cx0 = std::max(cx0, (int)std::floor(f.clip.x));
      cy0 = std::max(cy0, (int)std::floor(f.clip.y));
      cx1 = std::min(cx1, (int)std::ceil(f.clip.x + f.clip.w));
      cy1 = std::min(cy1, (int)std::ceil(f.clip.y + f.clip.h));
    }
    auto put = [&](int x, int y, int weight) {
      if (x < cx0 || y < cy0 || x >= cx1 || y >= cy1) return;
      const size_t i = (size_t)y * w + x;
      data[i] = energydiagram::mix565(touched[i] == mark ? data[i] : ground, colour, weight);
      touched[i] = mark;
    };
    PolygonRaster::Sink sink;
    // A fading fill's alpha by row (chartshapes::fadeAlpha).
    auto alphaAt = [&](int y) { return f.hasFade ? chartshapes::fadeAlpha(f.fade, y) : f.alpha; };
    sink.fullRun = [&](int16_t y, int16_t x0, int16_t x1) {
      const int a = alphaAt(y);
      for (int x = x0; x <= x1; x++) put(x, y, a);
    };
    sink.partial = [&](int16_t y, int16_t x, uint8_t c) { put(x, y, c * alphaAt(y) / 16); };
    PolygonRaster::fill(f.points, 0, (int16_t)(h - 1), sink);
  }

  void drawOn(ClippedCanvas16* canvas, int ox, int oy) const {
    int x0 = 0, y0 = 0, x1 = canvas->width(), y1 = canvas->height();
    if (canvas->clipActive()) {
      x0 = std::max(x0, (int)canvas->clipX());
      y0 = std::max(y0, (int)canvas->clipY());
      x1 = std::min(x1, (int)canvas->clipX() + canvas->clipW());
      y1 = std::min(y1, (int)canvas->clipY() + canvas->clipH());
    }
    x0 = std::max(x0, ox);
    y0 = std::max(y0, oy);
    x1 = std::min(x1, ox + w);
    y1 = std::min(y1, oy + h);
    uint16_t* buf = canvas->getBuffer();
    const int cw = canvas->width();
    for (int y = y0; y < y1; y++) {
      const size_t row = (size_t)(y - oy) * w;
      uint16_t* dst = buf + (size_t)y * cw;
      for (int x = x0; x < x1; x++) {
        const size_t i = row + (x - ox);
        if (touched[i] == mark) dst[x] = data[i];
      }
    }
  }
};

ChartPixels chartPixels;

}  // namespace

bool ColorScreenRenderer::renderChart(const ScreenObject& obj) {
  if (!obj.properties.chart) return false;
  const ChartProps& props = *obj.properties.chart;
  const ProjectConfig& project = projectLoader_.getProject();

  // The chart with the colours it has now (light or dark, ThemeVariant.h).
  chartcore::Chart chart = props.chart;
  for (size_t i = 0; i < chart.series.size() && i < props.seriesColor.size(); i++) chart.series[i].color = props.seriesColor[i].c_str();
  for (size_t i = 0; i < chart.zones.size() && i < props.zoneColor.size(); i++) chart.zones[i].color = props.zoneColor[i].c_str();
  const String textHex = obj.properties.textColor.isEmpty() ? String("#1d1b20") : obj.properties.textColor;
  const String mutedHex = props.mutedColor.isEmpty() ? String("#49454f") : props.mutedColor;
  const String gridHex = obj.properties.lineColor.isEmpty() ? String("#cac4d0") : obj.properties.lineColor;
  for (chartcore::Series& s : chart.series)
    if (s.color.empty()) s.color = "#6750a4";
  for (chartcore::Zone& z : chart.zones)
    if (z.color.empty()) z.color = gridHex.c_str();

  int fontSize = 14;
  for (const Font& f : project.fonts)
    if (f.id == props.fontId && f.size > 0) fontSize = f.size;
  // The days' names above the plot: the date font's size, else the label font's.
  const String& dateFontId = props.dateFontId.isEmpty() ? props.fontId : props.dateFontId;
  int dateFontSize = fontSize;
  for (const Font& f : project.fonts)
    if (f.id == dateFontId && f.size > 0) dateFontSize = f.size;

  // The history this board holds, in the recorder's days; what to say where there is none.
  const charthistory::History* history = chartFeed_ ? &chartFeed_->history() : nullptr;
  chartcore::LayoutOptions o;
  o.width = obj.width;
  o.height = obj.height;
  o.fontSize = fontSize;
  o.dateFontSize = dateFontSize;
  o.now = history && history->hasClock() ? chartFeed_->now() : 0;
  if (history) o.cal = history->calendar();
  o.decimal = project.decimalSeparator.isEmpty() ? "." : project.decimalSeparator.c_str();
  const chartcore::RangeView view = chartFeed_ ? chartFeed_->shownView(project) : chartcore::RangeView();
  const chartcore::Window window = chartcore::chartWindow(chart, view.range, o.now, o.cal, view.back);

  std::vector<chartcore::SeriesData> data;
  bool anyHeld = false;
  for (const chartcore::Series& s : chart.series) {
    std::vector<JsonDocument> docs;
    std::vector<JsonObjectConst> payloads;
    if (history) {
      chartcore::Chart one;
      one.series.push_back(s);
      for (const std::string& t : chartcore::historyTopicsFor(one, window)) {
        const std::string& raw = history->payload(t);
        if (raw.empty()) continue;
        docs.emplace_back();
        if (deserializeJson(docs.back(), raw)) {
          docs.pop_back();
          continue;
        }
        anyHeld = true;
      }
      for (const JsonDocument& d : docs) payloads.push_back(d.as<JsonObjectConst>());
    }
    data.push_back(chartcore::seriesData(s, payloads, o.cal));
  }
  const chartcore::Layout layout = chartcore::chartLayout(chart, window, data, o);

  // The shapes: into the chart's pixels over the screen's ground, then onto the canvas.
  chartshapes::Look look;
  look.grid = gridHex.c_str();
  // The line at now and a stack's NowPoint; the designer exports the theme's accent.
  look.now = props.nowColor.isEmpty() ? "#e53935" : props.nowColor.c_str();
  look.grey = !project.exportColorDepth.isEmpty() && project.exportColorDepth != "24bit";
  look.pulse = look.grey ? -1 : chartPulse_;
  // The NowPoints, for the next pulse step: the ring at its widest, and a pixel more.
  for (const chartcore::SeriesLayout& s : layout.series) {
    if (!s.hasNowPoint) continue;
    const int16_t r = 25;
    const PulseBox b{(int16_t)(obj.x + lround(s.nowPoint.first) - r), (int16_t)(obj.y + lround(s.nowPoint.second) - r), (int16_t)(2 * r), (int16_t)(2 * r)};
    bool known = false;
    for (const PulseBox& k : chartPulseBoxes_) known = known || (k.x == b.x && k.y == b.y);
    if (!known) chartPulseBoxes_.push_back(b);
  }
  const uint16_t ground = parseHexColor(currentScreenBgHex_);
  if (chartPixels.begin(obj.width, obj.height, ground)) {
    for (const chartshapes::Fill& f : chartshapes::chartShapes(layout, look)) chartPixels.fill(f, parseHexColor(String(f.colour.c_str())));
    chartPixels.drawOn(canvas_, obj.x, obj.y);
  }

  // The texts, as the designer's Text objects: in the chart's font, cut at
  // the chart's edges as the designer's canvas cuts them.
  const bool hadRegion = canvas_->regionActive();
  const int16_t prX = canvas_->regionX(), prY = canvas_->regionY(), prW = canvas_->regionW(), prH = canvas_->regionH();
  {
    int16_t x0 = obj.x, y0 = obj.y, x1 = obj.x + obj.width, y1 = obj.y + obj.height;
    if (hadRegion) {
      x0 = std::max<int16_t>(x0, prX);
      y0 = std::max<int16_t>(y0, prY);
      x1 = std::min<int16_t>(x1, prX + prW);
      y1 = std::min<int16_t>(y1, prY + prH);
    }
    canvas_->setRegion(x0, y0, std::max<int16_t>(0, x1 - x0), std::max<int16_t>(0, y1 - y0));
  }
  auto text = [&](const chartcore::Text& t) {
    ScreenObject box;
    box.type = "text";
    box.x = (int)lround(obj.x + t.x);
    box.y = (int)lround(obj.y + t.y);
    box.width = std::max(1, (int)lround(t.w));
    box.height = (int)t.h;
    box.properties.backgroundColor = "transparent";
    box.properties.borderColor = "";
    box.properties.textAlign = t.align.c_str();
    box.properties.fontId = t.date ? dateFontId : props.fontId;
    box.properties.color = t.tone == "text" ? textHex : mutedHex;
    drawTextBox(box, String(t.text.c_str()), false);
  };
  for (const chartcore::Text& t : layout.texts) text(t);
  const char* message = chart.series.empty()                               ? "Chart: add a series"
                        : !history || !history->recorderOnline()          ? "No recorder on this broker"
                        : !anyHeld                                         ? "No history yet"
                                                                           : nullptr;
  if (message) {
    chartcore::Text m;
    m.text = message;
    m.x = layout.plot.x;
    m.y = layout.plot.y + layout.plot.h / 2 - fontSize / 2.0;
    m.w = layout.plot.w;
    m.h = fontSize;
    m.align = "center";
    m.tone = "muted";
    text(m);
  }
  if (hadRegion) canvas_->setRegion(prX, prY, prW, prH);
  else canvas_->clearRegion();
  return true;
}
