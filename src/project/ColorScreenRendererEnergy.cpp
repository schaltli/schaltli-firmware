// The energy diagram drawn (designer docs/device-contract.md 2.8): every
// place and every shape's outline from EnergyDiagram.h, filled by
// PolygonRaster and mixed in RGB565 in whole numbers exactly as the
// designer's renderer does (components/canvas/renderers/
// render-energy-diagram.ts), so the two are the same pixels.
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
struct DiagramPixels {
  int w = 0, h = 0;
  uint16_t ground = 0xFFFF;
  uint16_t* data = nullptr;
  uint8_t* touched = nullptr;
  size_t capacity = 0;

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
    }
    if (capacity < n) {
      w = h = 0;
      return;
    }
    memset(touched, 0, n);
  }

  void put(int x, int y, uint16_t colour, int weight) {
    if (x < 0 || y < 0 || x >= w || y >= h) return;
    const size_t i = (size_t)y * w + x;
    data[i] = ed::mix565(touched[i] ? data[i] : ground, colour, weight);
    touched[i] = 1;
  }

  void fill(const std::vector<ed::Point8>& outline, uint16_t colour, int alpha = 256) {
    std::vector<PolygonRaster::Point> pts;
    pts.reserve(outline.size());
    for (const ed::Point8& p : outline) pts.push_back({p.x, p.y});
    PolygonRaster::Sink sink;
    sink.fullRun = [&](int16_t y, int16_t x0, int16_t x1) {
      for (int x = x0; x <= x1; x++) put(x, y, colour, alpha);
    };
    sink.partial = [&](int16_t y, int16_t x, uint8_t c) { put(x, y, colour, c * alpha / 16); };
    PolygonRaster::fill(pts, 0, (int16_t)(h - 1), sink);
  }

  void drawOn(ClippedCanvas16* canvas, int ox, int oy) const {
    for (int y = 0; y < h; y++)
      for (int x = 0; x < w; x++) {
        const size_t i = (size_t)y * w + x;
        if (touched[i]) canvas->drawPixel(ox + x, oy + y, data[i]);
      }
  }
};
DiagramPixels diagramPixels;
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

  DiagramPixels& px = diagramPixels;
  px.begin(obj.width, obj.height, ground);
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
      auto column = [&](int x) {
        const std::array<int, 3> c = ed::waveColour(base, groundRgb, x, wavePhase);
        return ed::rgb565(c[0], c[1], c[2]);
      };
      std::vector<PolygonRaster::Point> pts;
      for (const ed::Point8& p : ed::bandOutline(band)) pts.push_back({p.x, p.y});
      PolygonRaster::Sink sink;
      sink.fullRun = [&](int16_t y, int16_t x0, int16_t x1) {
        for (int x = x0; x <= x1; x++) px.put(x, y, column(x), 256);
      };
      sink.partial = [&](int16_t y, int16_t x, uint8_t c) { px.put(x, y, column(x), c * 16); };
      PolygonRaster::fill(pts, 0, (int16_t)(obj.height - 1), sink);
    }
    px.fill(ed::rectOutline(layout.bus.x, layout.bus.y, layout.bus.width, layout.bus.height), parseHexColor(e.busColor));
    for (const ed::Bar& b : layout.bars) {
      const double r = std::min(3.0, b.box.height / 2);
      px.fill(ed::barOutline(b.box.x, b.box.y, b.box.width, b.box.height, r, b.side == ed::Side::Left), colourFor(b.id));
    }
    boxes = ed::sankeyBoxes(layout, nodes, size, m);
  } else {
    const ed::FlowLayout layout = ed::flowLayout(nodes, resolved);
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
      for (const ed::Point& d : ed::dotsOnLine(line, phase, r->gives, r->watts)) px.fill(ed::discOutline(d.x, d.y, ed::DOT_R), colour);
    }
    px.fill(ed::discOutline(layout.busX, layout.busY, layout.busR), parseHexColor(e.busColor));
    for (const ed::FlowNode& fn : layout.nodes) {
      const ed::Resolved* r = nullptr;
      for (const ed::Resolved& x : resolved)
        if (x.id == fn.id) r = &x;
      const uint16_t colour = parseHexColor(fn.live ? colourOf(*r) : e.idleColor);
      if (fn.hasRing) {
        px.fill(ed::ringOutline(fn.x, fn.y, fn.ringR + 2.5, fn.ringR - 2.5), parseHexColor(e.idleColor));
        const int sweep = (int)ed::jsRound(fn.ringFraction * 360);
        if (sweep > 0) px.fill(ed::arcOutline(fn.x, fn.y, fn.ringR, 5, sweep), parseHexColor(colourOf(*r)));
      }
      // The circle: a ring 3 px wide round the ground.
      px.fill(ed::discOutline(fn.x, fn.y, fn.r + 1.5), colour);
      px.fill(ed::discOutline(fn.x, fn.y, fn.r - 1.5), ground);
    }
    boxes = ed::flowBoxes(layout, m);
  }
  canvas_->setClip(obj.x, obj.y, obj.width, obj.height);
  px.drawOn(canvas_, obj.x, obj.y);

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
