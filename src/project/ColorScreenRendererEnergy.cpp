// The energy diagram drawn (designer docs/device-contract.md 2.8): every
// place and every shape's outline from EnergyDiagram.h, filled by
// PolygonRaster and mixed in RGB565 in whole numbers exactly as the
// designer's renderer does (components/canvas/renderers/
// render-energy-diagram.ts), so the two are the same pixels.
#include <algorithm>
#include <array>
#include <cmath>

#include "ColorScreenRenderer.h"
#include "EnergyDiagram.h"
#include "PolygonRaster.h"

namespace ed = energydiagram;

namespace {

double numberOf(const String& raw, bool& ok) {
  String s = raw;
  s.trim();
  ok = false;
  if (s.isEmpty()) return 0;
  char* end = nullptr;
  const double v = strtod(s.c_str(), &end);
  ok = end != nullptr && *end == '\0' && std::isfinite(v);
  return ok ? v : 0;
}

}  // namespace

// The diagram's nodes for the core, and what their topics say now.
void ColorScreenRenderer::energyInputs(const EnergyDiagramProps& e, std::vector<ed::Node>& nodes, std::vector<ed::Value>& values) const {
  nodes.clear();
  values.clear();
  for (const EnergyNodeProps& n : e.nodes) {
    ed::Node node;
    node.id = n.id.c_str();
    node.role = ed::roleFrom(n.role.c_str());
    node.x = n.x;
    node.y = n.y;
    node.reverseSign = n.reverseSign;
    node.lines = (int)n.lines.size();
    nodes.push_back(node);
    ed::Value v;
    if (node.role != ed::Role::Bus && node.role != ed::Role::Rest && !n.topic.isEmpty()) {
      const auto raw = liveSourceValue({"topic", n.topic.c_str()});
      bool ok = false;
      if (raw) v.watts = numberOf(String(raw->c_str()), ok);
      v.hasWatts = ok;
    }
    if (node.role == ed::Role::Storage && !n.chargeTopic.isEmpty()) {
      const auto raw = liveSourceValue({"topic", n.chargeTopic.c_str()});
      bool ok = false;
      if (raw) v.charge = numberOf(String(raw->c_str()), ok);
      v.hasCharge = ok;
    }
    values.push_back(v);
  }
}

// The diagram's shapes as the designer draws them (render-energy-diagram.ts
// DiagramPixels): RGB565 pixels of the diagram's own size, each shape an
// outline from the core filled by PolygonRaster, mixed in whole numbers. A
// pixel no shape has touched yet mixes into the screen's ground; one never
// touched is left as the canvas has it. Kept in PSRAM between renders.
namespace {
uint32_t profPuts = 0, profFillUs = 0, profFills = 0;
struct DiagramPixels {
  int w = 0, h = 0;
  uint16_t ground = 0xFFFF;
  uint16_t* data = nullptr;
  // A pixel was touched this picture when it holds `mark`: no clearing of
  // the whole buffer per picture (14 ms, 2026-10-12) - only when it wraps.
  uint8_t* touched = nullptr;
  uint8_t mark = 1;
  size_t capacity = 0;
  // What the picture shows (the diagram, its size, ground, variant and
  // values): the same again - a swipe back to it, a redraw for something
  // else - is the picture as it is, no shape drawn anew. The user's idea,
  // 2026-10-12, for a swipe that waited on the diagram.
  std::string key;
  std::string id;  // the diagram it was last drawn for
  bool reuse = false;

  void begin(int width, int height, uint16_t groundColour) {
    w = width > 0 ? width : 0;
    h = height > 0 ? height : 0;
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
      key.clear();
    }
    if (capacity < n) {
      w = h = 0;
      return;
    }
    if (++mark == 0) {
      memset(touched, 0, capacity);
      mark = 1;
    }
  }

  void put(int x, int y, uint16_t colour, int weight) {
    profPuts++;
    if (x < 0 || y < 0 || x >= w || y >= h) return;
    const size_t i = (size_t)y * w + x;
    data[i] = ed::mix565(touched[i] == mark ? data[i] : ground, colour, weight);
    touched[i] = mark;
  }

  void fill(const std::vector<ed::Point8>& outline, uint16_t colour, int alpha = 256) {
    if (reuse) return;
    const uint32_t t0 = micros();
    profFills++;
    std::vector<PolygonRaster::Point> pts;
    pts.reserve(outline.size());
    for (const ed::Point8& p : outline) pts.push_back({p.x, p.y});
    PolygonRaster::Sink sink;
    sink.fullRun = [&](int16_t y, int16_t x0, int16_t x1) {
      for (int x = x0; x <= x1; x++) put(x, y, colour, alpha);
    };
    sink.partial = [&](int16_t y, int16_t x, uint8_t c) { put(x, y, colour, c * alpha / 16); };
    PolygonRaster::fill(pts, 0, (int16_t)(h - 1), sink);
    profFillUs += micros() - t0;
  }

  // Straight into the canvas's buffer, within its clip: a drawPixel per
  // pixel of the diagram cost 39 ms (2026-10-12).
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
// One picture per diagram, the three last drawn: a Sankey on one screen and
// a Flow on the next would otherwise take the one picture from each other
// on every swipe between them (the user, 2026-10-12).
constexpr int KEPT_PICTURES = 3;
DiagramPixels diagramPictures[KEPT_PICTURES];
uint32_t pictureUsed[KEPT_PICTURES] = {0};
uint32_t pictureClock = 0;

// The picture kept for `id`, else the one used longest ago.
DiagramPixels& pictureFor(const String& id) {
  int pick = 0;
  for (int i = 0; i < KEPT_PICTURES; i++) {
    if (diagramPictures[i].id == id.c_str()) {
      pick = i;
      break;
    }
    if (pictureUsed[i] < pictureUsed[pick]) pick = i;
  }
  pictureUsed[pick] = ++pictureClock;
  return diagramPictures[pick];
}
}  // namespace

bool ColorScreenRenderer::renderEnergyDiagram(const ScreenObject& obj, float wavePhase, const EnergyDotPhases* dots) {
  if (!obj.properties.energy) return false;
  const EnergyDiagramProps& e = *obj.properties.energy;
  energyDrawCount_++;
  std::vector<ed::Node> nodes;
  std::vector<ed::Value> values;
  energyInputs(e, nodes, values);
  const std::vector<ed::Resolved> resolved = ed::resolve(nodes, values);
  const float ox = obj.x, oy = obj.y;
  const uint16_t ground = parseHexColor(currentScreenBgHex_);
  const std::array<int, 3> groundRgb = ed::expand565(ground);

  auto nodeOf = [&](const std::string& id) -> const EnergyNodeProps* {
    for (const EnergyNodeProps& n : e.nodes)
      if (id == n.id.c_str()) return &n;
    return nullptr;
  };
  auto colourOf = [&](const ed::Resolved& r) -> String {
    if (r.role == ed::Role::Storage) return r.gives ? e.dischargeColor : e.chargeColor;
    const EnergyNodeProps* n = nodeOf(r.id);
    return n && !n->color.isEmpty() ? n->color : e.textColor;
  };
  // What {watts} of a node says: nothing while its topic has not spoken.
  auto known = [&](const ed::Resolved& r) {
    if (r.role == ed::Role::Rest) return true;
    for (size_t i = 0; i < nodes.size(); i++)
      if (nodes[i].id == r.id) return values[i].hasWatts;
    return false;
  };

  const uint32_t tStart = micros();
  profPuts = profFillUs = profFills = 0;
  DiagramPixels& px = pictureFor(obj.id);
  // Moving dots drawn in (a test's still frame) are never kept.
  std::string key;
  if (dots == nullptr) {
    key = std::string(obj.id.c_str()) + "|" + std::to_string(obj.width) + "x" + std::to_string(obj.height) + "|" + std::to_string(ground) + "|" +
          (projectLoader_.getProject().themeDark ? "d" : "l") + "|" + std::to_string(wavePhase);
    for (const ed::Value& v : values)
      key += "|" + (v.hasWatts ? std::to_string(v.watts) : std::string("-")) + "/" + (v.hasCharge ? std::to_string(v.charge) : std::string("-"));
  }
  px.reuse = !key.empty() && key == px.key && px.w == obj.width && px.h == obj.height;
  if (!px.reuse) {
    px.begin(obj.width, obj.height, ground);
    px.key = key;
    px.id = obj.id.c_str();
  }
  const uint32_t tBegin = micros();
  const ed::LabelMetrics m{(double)e.fontSize, (double)e.valueFontSize};
  ed::DiagramBoxes boxes;
  if (e.sankey) {
    const ed::Size size{(double)obj.width, (double)obj.height};
    const ed::SankeyLayout layout = ed::sankeyLayout(resolved, size, e.fullScale > 0 ? e.fullScale : 1000);
    auto colourFor = [&](const std::string& id) -> uint16_t {
      for (const ed::Resolved& r : resolved)
        if (r.id == id) return parseHexColor(r.watts > 0 ? colourOf(r) : e.idleColor);
      return parseHexColor(e.textColor);
    };
    // The bands first, each column one colour of its wave (decision 10).
    for (const ed::Band& band : layout.bands) {
      const std::array<int, 3> base = ed::expand565(colourFor(band.id));
      // A column's colour by its x alone, worked out once per column: per
      // pixel, the cos in software doubles cost a Sankey 3 s (2026-10-12).
      static std::vector<uint16_t> columns;
      columns.assign((size_t)std::max(0, obj.width), 0);
      static std::vector<uint8_t> known;
      known.assign((size_t)std::max(0, obj.width), 0);
      auto column = [&](int x) -> uint16_t {
        if (x < 0 || x >= obj.width) return 0;
        if (!known[x]) {
          const std::array<int, 3> c = ed::waveColour(base, groundRgb, x, wavePhase);
          columns[x] = ed::rgb565(c[0], c[1], c[2]);
          known[x] = 1;
        }
        return columns[x];
      };
      std::vector<PolygonRaster::Point> pts;
      for (const ed::Point8& p : ed::bandOutline(band)) pts.push_back({p.x, p.y});
      PolygonRaster::Sink sink;
      // Whole pixels of a row straight in: at full weight a pixel is its
      // column's colour, whatever was under it.
      sink.fullRun = [&](int16_t y, int16_t x0, int16_t x1) {
        if (y < 0 || y >= px.h) return;
        const int a = std::max<int>(0, x0), b = std::min<int>(px.w - 1, x1);
        uint16_t* data = px.data + (size_t)y * px.w;
        uint8_t* touched = px.touched + (size_t)y * px.w;
        for (int x = a; x <= b; x++) {
          data[x] = column(x);
          touched[x] = px.mark;
        }
      };
      sink.partial = [&](int16_t y, int16_t x, uint8_t c) { px.put(x, y, column(x), c * 16); };
      if (!px.reuse) PolygonRaster::fill(pts, 0, (int16_t)(obj.height - 1), sink);
    }
    px.fill(ed::rectOutline(layout.bus.x, layout.bus.y, layout.bus.width, layout.bus.height), parseHexColor(e.busColor));
    for (const ed::Bar& b : layout.bars) {
      const double r = std::min(3.0, b.box.height / 2);
      px.fill(ed::barOutline(b.box.x, b.box.y, b.box.width, b.box.height, r, b.side == ed::Side::Left), colourFor(b.id));
    }
    boxes = ed::sankeyBoxes(layout, nodes, size, m);
  } else {
    const ed::FlowLayout layout = ed::flowLayout(nodes, resolved);
    std::vector<std::pair<ed::PathDot, uint16_t>> storageDots;
    for (const ed::FlowLine& line : layout.lines) {
      const ed::Resolved* r = nullptr;
      for (const ed::Resolved& x : resolved)
        if (x.id == line.id) r = &x;
      const uint16_t colour = parseHexColor(line.live ? colourOf(*r) : e.idleColor);
      // A live line at half strength, under its dots.
      px.fill(ed::capsuleOutline(line.x0, line.y0, line.x1, line.y1, line.width), colour, line.live ? 128 : 256);
      // Moving, the board draws the dots over the still picture.
      if (!line.live || (e.motion && dots == nullptr)) continue;
      double phase = 0;
      if (dots) {
        for (const auto& d : *dots)
          if (d.first == line.id) phase = d.second;
      }
      const ed::FlowNode* fn = nullptr;
      for (const ed::FlowNode& x : layout.nodes)
        if (x.id == line.id) fn = &x;
      if (r->role == ed::Role::Storage && fn && fn->hasRing) {
        // A storage's dots run on into its track, drawn over it below.
        for (const ed::PathDot& d : ed::storageDots(line, *fn, phase, r->gives, r->watts)) {
          if (d.onLine) px.fill(ed::discOutline(d.x, d.y, ed::DOT_R), colour);
          else storageDots.push_back({d, colour});
        }
      } else {
        for (const ed::Point& d : ed::dotsOnLine(line, phase, r->gives, r->watts)) px.fill(ed::discOutline(d.x, d.y, ed::DOT_R), colour);
      }
    }
    px.fill(ed::discOutline(layout.busX, layout.busY, layout.busR), parseHexColor(e.busColor));
    for (const ed::FlowNode& fn : layout.nodes) {
      const ed::Resolved* r = nullptr;
      for (const ed::Resolved& x : resolved)
        if (x.id == fn.id) r = &x;
      const uint16_t colour = parseHexColor(fn.live ? colourOf(*r) : e.idleColor);
      if (fn.hasRing) {
        // The track: opaque, the bar's colour mixed into the ground beforehand,
        // over the line's round end.
        const double h = ed::FLOW_RING_W / 2;
        const std::array<int, 3> track = ed::trackColour(ed::expand565(parseHexColor(colourOf(*r))), groundRgb);
        px.fill(ed::ringOutline(fn.x, fn.y, fn.ringR + h, fn.ringR - h), ed::rgb565(track[0], track[1], track[2]));
        const int sweep = (int)ed::jsRound(fn.ringFraction * 360);
        if (sweep > 0) px.fill(ed::arcOutline(fn.x, fn.y, fn.ringR, ed::FLOW_RING_W, sweep), parseHexColor(colourOf(*r)));
      }
      // The circle: a ring 3 px wide round the ground.
      px.fill(ed::discOutline(fn.x, fn.y, fn.r + 1.5), colour);
      px.fill(ed::discOutline(fn.x, fn.y, fn.r - 1.5), ground);
    }
    for (const auto& sd : storageDots) {
      const ed::PathDot& d = sd.first;
      const uint16_t ink = d.onBar ? ed::mix565(sd.second, 0xFFFF, (int)ed::jsRound(ed::BAR_DOT_LIGHT * 256)) : sd.second;
      px.fill(ed::discOutline(d.x, d.y, ed::DOT_R), ink, (int)ed::jsRound(d.alpha * 256));
    }
    boxes = ed::flowBoxes(layout, m);
  }
  const uint32_t tShapes = micros();
  canvas_->setClip(obj.x, obj.y, obj.width, obj.height);
  px.drawOn(canvas_, obj.x, obj.y);
  const uint32_t tDrawn = micros();

  // Names, lines and icons, at the core's boxes (contract 2.8).
  const ProjectConfig& project = projectLoader_.getProject();
  placeholders::Separators separators;
  separators.decimal = project.decimalSeparator.c_str();
  separators.thousands = project.thousandsSeparator.c_str();
  auto label = [&](const ed::LabelBox& box, const String& name, const EnergyNodeProps* node, const ed::Resolved* r) {
    ScreenObject t;
    t.type = "text";
    t.x = (int)lroundf(ox + box.x);
    t.y = (int)lroundf(oy + box.y);
    t.width = (int)lroundf(box.width);
    t.properties.backgroundColor = "transparent";
    t.properties.borderColor = "";
    t.properties.textAlign = box.align == ed::Align::Left ? "left" : box.align == ed::Align::Right ? "right" : "center";
    t.properties.fontId = e.fontId;
    t.properties.color = e.mutedColor;
    t.height = e.fontSize;
    drawTextBox(t, name, false);
    t.y += (int)ed::rowStep(e.fontSize);
    if (!node || !r) return;
    const bool has = known(*r);
    const double watts = r->role == ed::Role::Storage && r->gives ? -r->watts : r->watts;
    for (size_t i = 0; i < node->lines.size() && i < (size_t)ed::MAX_NODE_LINES; i++) {
      const EnergyLine& line = node->lines[i];
      const bool first = i == 0;
      t.properties.fontId = first ? e.valueFontId : e.fontId;
      t.properties.color = first ? e.textColor : e.mutedColor;
      String text;
      if (!line.liveText.isEmpty()) {
        const std::string filled = ed::withWatts(line.liveText.c_str(), has, watts);
        text = String(livevalue::resolveLiveText(filled, line.liveValues, [this](const livevalue::Source& s) { return liveSourceValue(s); }, separators).c_str());
      } else {
        text = resolvePlaceholders(String(ed::withWatts(line.text.c_str(), has, watts).c_str()));
      }
      drawTextBox(t, text, false);
      t.y += (int)ed::rowStep(first ? e.valueFontSize : e.fontSize);
    }
  };
  for (const EnergyNodeProps& n : e.nodes) {
    if (n.role == "bus" && boxes.hasBus) label(boxes.bus, n.name, nullptr, nullptr);
  }
  for (const ed::NodeBoxes& b : boxes.nodes) {
    const EnergyNodeProps* node = nodeOf(b.id);
    const ed::Resolved* r = nullptr;
    for (const ed::Resolved& x : resolved)
      if (x.id == b.id) r = &x;
    if (!node || !r) continue;
    if (node->hasIcon) {
      const String& path = r->watts > 0 ? (r->role == ed::Role::Storage ? (r->gives ? node->icon.discharge : node->icon.charge) : node->icon.live) : node->icon.muted;
      if (!path.isEmpty()) ColorAssetLoader::drawBMPToCanvas(canvas_, path, (int16_t)lroundf(ox + b.icon.x), (int16_t)lroundf(oy + b.icon.y));
    }
    label(b.label, node->name, node, r);
  }
  const uint32_t tEnd = micros();
  energyProfile_ = "begin " + String((tBegin - tStart) / 1000.0f, 1) + " ms, shapes " + String((tShapes - tBegin) / 1000.0f, 1) + " ms, onto the canvas " +
                   String((tDrawn - tShapes) / 1000.0f, 1) + " ms, labels and icons " + String((tEnd - tDrawn) / 1000.0f, 1) + " ms; " + String(profFills) + " fills " + String(profFillUs / 1000.0f, 1) + " ms, " + String(profPuts) + " pixels";
  canvas_->clearClip();
  return true;
}

std::vector<ColorScreenRenderer::EnergyAnimation> ColorScreenRenderer::energyAnimations(int screenIndex) {
  std::vector<EnergyAnimation> out;
  const ProjectConfig& project = projectLoader_.getProject();
  if (screenIndex < 0 || screenIndex >= (int)project.screens.size()) return out;
  const std::array<int, 3> groundRgb = ed::expand565(parseHexColor(currentScreenBgHex_));
  for (const ScreenObject& obj : project.screens[screenIndex].objects) {
    if (obj.type != "energy-diagram" || !obj.properties.energy || !obj.properties.energy->motion) continue;
    const EnergyDiagramProps& e = *obj.properties.energy;
    std::vector<ed::Node> nodes;
    std::vector<ed::Value> values;
    energyInputs(e, nodes, values);
    const std::vector<ed::Resolved> resolved = ed::resolve(nodes, values);
    EnergyAnimation a;
    a.id = obj.id;
    a.sankey = e.sankey;
    a.originX = obj.x;
    a.ground = groundRgb;
    auto colourOf = [&](const ed::Resolved& r) -> String {
      if (r.role == ed::Role::Storage) return r.gives ? e.dischargeColor : e.chargeColor;
      for (const EnergyNodeProps& n : e.nodes)
        if (r.id == n.id.c_str() && !n.color.isEmpty()) return n.color;
      return e.textColor;
    };
    if (e.sankey) {
      const ed::SankeyLayout layout = ed::sankeyLayout(resolved, {(double)obj.width, (double)obj.height}, e.fullScale > 0 ? e.fullScale : 1000);
      const int16_t cy0 = obj.y < 0 ? 0 : obj.y, cy1 = obj.y + obj.height - 1;
      const int16_t left = obj.x, right = obj.x + obj.width - 1;
      for (const ed::Band& band : layout.bands) {
        uint16_t colour = parseHexColor(e.textColor);
        for (const ed::Resolved& r : resolved)
          if (r.id == band.id) colour = parseHexColor(r.watts > 0 ? colourOf(r) : e.idleColor);
        const uint8_t index = (uint8_t)a.bandRgb.size();
        a.bandRgb.push_back(ed::expand565(colour));
        // The band's outline as the still picture has it, moved by whole pixels.
        std::vector<PolygonRaster::Point> o;
        for (const ed::Point8& p : ed::bandOutline(band)) o.push_back({p.x + obj.x * 8, p.y + obj.y * 8});
        PolygonRaster::Sink sink;
        sink.fullRun = [&](int16_t y, int16_t x0, int16_t x1) {
          const int16_t lo = x0 < left ? left : x0, hi = x1 > right ? right : x1;
          if (hi >= lo) a.runs.push_back({y, lo, hi, index});
        };
        sink.partial = [](int16_t, int16_t, uint8_t) {};
        PolygonRaster::fill(o, cy0, cy1, sink);
      }
    } else {
      const ed::FlowLayout layout = ed::flowLayout(nodes, resolved);
      for (const ed::FlowLine& line : layout.lines) {
        if (!line.live) continue;
        for (const ed::Resolved& r : resolved) {
          if (r.id != line.id) continue;
          EnergyAnimation::Line l;
          l.id = line.id;
          l.line = line;
          l.line.x0 += obj.x;
          l.line.x1 += obj.x;
          l.line.y0 += obj.y;
          l.line.y1 += obj.y;
          l.gives = r.gives;
          l.watts = r.watts;
          l.colour = parseHexColor(colourOf(r));
          for (const ed::FlowNode& fn : layout.nodes) {
            if (fn.id != line.id || !fn.hasRing || r.role != ed::Role::Storage) continue;
            l.storage = true;
            l.node = fn;
            l.node.x += obj.x;
            l.node.y += obj.y;
          }
          a.lines.push_back(l);
        }
      }
    }
    out.push_back(a);
  }
  return out;
}

// A moving dot, from a mask: its centre to the nearest eighth of a pixel,
// each of the 64 masks rastered once (2026-10-12: rastering each dot anew
// cost 46 ms a step for 31 dots - doubles are software on this chip). Only
// the still picture is compared with the designer; a moving dot need only
// look the same, and it does.
void ColorScreenRenderer::energyDotPixels(double cx, double cy, const std::function<void(int16_t, int16_t, uint8_t)>& pixel) {
  struct Pix {
    int8_t dx, dy;
    uint8_t c;
  };
  static std::vector<Pix> masks[64];
  static bool made[64] = {false};
  const int32_t qx = ed::toEighths(cx), qy = ed::toEighths(cy);
  const int32_t ix = qx >> 3, iy = qy >> 3;  // floor, also below nought
  const int key = (qx & 7) * 8 + (qy & 7);
  if (!made[key]) {
    std::vector<PolygonRaster::Point> pts;
    for (const ed::Point8& p : ed::discOutline(8 + (qx & 7) / 8.0, 8 + (qy & 7) / 8.0, ed::DOT_R)) pts.push_back({p.x, p.y});
    PolygonRaster::Sink sink;
    std::vector<Pix>& mask = masks[key];
    sink.fullRun = [&](int16_t y, int16_t x0, int16_t x1) {
      for (int16_t x = x0; x <= x1; x++) mask.push_back({(int8_t)(x - 8), (int8_t)(y - 8), 16});
    };
    sink.partial = [&](int16_t y, int16_t x, uint8_t c) { mask.push_back({(int8_t)(x - 8), (int8_t)(y - 8), c}); };
    PolygonRaster::fill(pts, 0, 20, sink);
    made[key] = true;
  }
  for (const Pix& p : masks[key]) pixel((int16_t)(ix + p.dx), (int16_t)(iy + p.dy), p.c);
}

uint16_t ColorScreenRenderer::energyBlend(uint16_t under, uint16_t over, uint8_t coverage) {
  return ed::mix565(under, over, coverage * 16);
}

uint16_t ColorScreenRenderer::energyWaveColour(const EnergyAnimation& a, uint8_t band, int16_t x, double wave) {
  const std::array<int, 3> c = ed::waveColour(a.bandRgb[band], a.ground, x - a.originX, wave);
  return ed::rgb565(c[0], c[1], c[2]);
}

const ButtonAction* ColorScreenRenderer::energyActionAt(const ScreenObject& obj, double x, double y) const {
  if (!obj.properties.energy) return nullptr;
  const EnergyDiagramProps& e = *obj.properties.energy;
  std::vector<ed::Node> nodes;
  std::vector<ed::Value> values;
  energyInputs(e, nodes, values);
  const std::string id = ed::nodeAtTap(e.sankey, nodes, ed::resolve(nodes, values), {(double)obj.width, (double)obj.height},
                                       e.fullScale > 0 ? e.fullScale : 1000, x, y);
  if (id.empty()) return nullptr;
  for (const EnergyNodeProps& n : e.nodes)
    if (id == n.id.c_str()) return n.action.type.isEmpty() ? nullptr : &n.action;
  return nullptr;
}
