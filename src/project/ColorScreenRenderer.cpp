#include "ColorScreenRenderer.h"

#include "ObjectTypes.h"

#include "SwitchShape.h"
#include "TextField.h"
#include "RenderProfile.h"
#include <esp_cpu.h>
#include "ArcRaster.h"
#include "LevelGlow.h"
#include "ArcSinTable.h"  // ARC_SIN_SCALE - Skala von ArcRaster::direction()
#include "Placeholders.h"
#include "DeviceInfo.h"  // DEVICE_MODEL, this board's own
#include "ScreenGraphProject.h"
#include <algorithm>

// Matches JavaScript's Math.round() exactly, including its tie-breaking
// direction (halves round toward +infinity) - see schaltli-eink's
// ScreenRenderer.cpp's identical helper for the full rationale (a naive
// roundf() port disagreed with the designer's center-align formula by 1px
// for negative odd width differences).
static inline int16_t jsRound(float v) {
  return (int16_t)floorf(v + 0.5f);
}

ColorScreenRenderer::ColorScreenRenderer(IProjectLoader& projectLoader, ClippedCanvas16* canvas)
  : projectLoader_(projectLoader), canvas_(canvas) {}

uint16_t ColorScreenRenderer::parseHexColor(const String& colorStr, bool* isTransparent) const {
  if (isTransparent) *isTransparent = false;

  String s = colorStr;
  s.trim();
  s.toLowerCase();

  if (s.isEmpty() || s == "transparent" || s == "#ffffff00" || s == "#00000000" ||
      s == "rgba(255,255,255,0)" || s == "rgba(0,0,0,0)") {
    if (isTransparent) *isTransparent = true;
    return 0;
  }
  // "#rrggbbaa" with alpha byte 00 is also fully transparent.
  if (s.length() == 9 && s.charAt(0) == '#') {
    String alpha = s.substring(7, 9);
    if (alpha == "00") {
      if (isTransparent) *isTransparent = true;
    }
  }

  if (s == "black") return 0x0000;
  if (s == "white") return 0xFFFF;

  if (s.length() >= 7 && s.charAt(0) == '#') {
    uint8_t r = strtol(s.substring(1, 3).c_str(), nullptr, 16);
    uint8_t g = strtol(s.substring(3, 5).c_str(), nullptr, 16);
    uint8_t b = strtol(s.substring(5, 7).c_str(), nullptr, 16);
    return ((uint16_t)(r & 0xF8) << 8) | ((uint16_t)(g & 0xFC) << 3) | (b >> 3);
  }

  // Unrecognized - black, same fallback the e-paper firmware's parseColor()
  // uses for anything it can't parse.
  return 0x0000;
}

// Looked up by the DDF's own font id, not by an internalName mapped onto a
// compiled-in table. That table was the bug: it named four u8g2 arrays with
// 191 glyphs each while the DDF published .bdf files with 756, so a euro
// sign rendered in the designer and vanished on the glass. It also fell
// back silently, so a project naming an unknown font drew in helvR18
// without a word. This still falls back - a wrong-looking label beats a
// blank screen - but says so once, and the fallback is the DDF's own first
// font rather than a hardcoded guess.
namespace {

// Every font id actually named by an object, children included - a
// tab-control holds panels which hold ordinary objects, and a label three
// levels down needs its font as much as one at the top.
void collectFontIds(const std::vector<ScreenObject>& objects, std::vector<String>& out) {
  for (const auto& obj : objects) {
    const String& id = obj.properties.fontId;
    if (!id.isEmpty()) {
      bool seen = false;
      for (const auto& existing : out) {
        if (existing == id) {
          seen = true;
          break;
        }
      }
      if (!seen) out.push_back(id);
    }
    if (!obj.children.empty()) collectFontIds(obj.children, out);
  }
}

}  // namespace

void ColorScreenRenderer::preloadFonts() {
  if (!projectLoader_.isLoaded()) return;
  const ProjectConfig& project = projectLoader_.getProject();

  // What the project *uses*, not what the DDF declares. Parsing one .bdf
  // costs about 275ms and it has to happen before the first render, so
  // preloading everything on offer puts the whole DDF's font list into the
  // boot time - a second per four fonts, paid by every project whether it
  // names them or not. A project typically uses one or two, and this is
  // what makes it safe to be generous about the fonts a DDF offers.
  std::vector<String> used;
  for (const auto& screen : project.screens) {
    collectFontIds(screen.objects, used);
  }
  // The fallback in fontFor() has to be there too, or the first object with
  // an unknown font would parse one mid-render - exactly what this avoids.
  const String fallback = fontStore_.defaultFontId();
  if (!fallback.isEmpty()) {
    bool seen = false;
    for (const auto& id : used) {
      if (id == fallback) {
        seen = true;
        break;
      }
    }
    if (!seen) used.push_back(fallback);
  }

  const uint32_t start = millis();
  for (const String& id : used) fontStore_.get(id);
  Serial.printf("[ddf-fonts] preloaded %u of %u declared font(s) in %ums\n", (unsigned)used.size(),
                (unsigned)project.fonts.size(), (unsigned)(millis() - start));
}

const BdfFont* ColorScreenRenderer::fontFor(const String& fontId) {
  const BdfFont* font = fontStore_.get(fontId);
  if (font != nullptr) return font;
  return fontStore_.get(fontStore_.defaultFontId());
}

int16_t ColorScreenRenderer::inkOverhang(const BdfFont* font, const String& text,
                                        int16_t nominalAscent) const {
  if (font == nullptr) return 0;
  const int16_t over = (int16_t)(font->inkAscentOf(text) - nominalAscent);
  return over > 0 ? over : 0;
}

int ColorScreenRenderer::getFontAscentById(const String& fontId) const {
  const ProjectConfig& project = projectLoader_.getProject();
  for (const Font& font : project.fonts) {
    if (font.id == fontId) return font.ascent;
  }
  return 14;
}

int ColorScreenRenderer::getFontDescentById(const String& fontId) const {
  const ProjectConfig& project = projectLoader_.getProject();
  for (const Font& font : project.fonts) {
    if (font.id == fontId) return font.descent;
  }
  return 4;
}

int ColorScreenRenderer::getFontSizeById(const String& fontId) const {
  const ProjectConfig& project = projectLoader_.getProject();
  for (const Font& font : project.fonts) {
    if (font.id == fontId) return font.size;
  }
  return 18;
}

String ColorScreenRenderer::formatNumber(const String& valueStr, int decimals, const String& thousandsSeparator) const {
  float value = valueStr.toFloat();
  String formatted = String(value, decimals);

  if (!thousandsSeparator.isEmpty()) {
    int decimalPos = formatted.indexOf('.');
    if (decimalPos == -1) decimalPos = formatted.length();

    int insertPos = decimalPos - 3;
    while (insertPos > 0) {
      if (insertPos == 1 && formatted.charAt(0) == '-') break;
      formatted = formatted.substring(0, insertPos) + thousandsSeparator + formatted.substring(insertPos);
      insertPos -= 3;
      decimalPos += thousandsSeparator.length();
    }
  }
  return formatted;
}

float ColorScreenRenderer::interpolateCalibration(float value, const std::vector<CalibrationPoint>& points) const {
  if (points.empty()) return value;
  if (points.size() == 1) return points[0].barSizePercent;

  std::vector<CalibrationPoint> sortedPoints = points;
  std::sort(sortedPoints.begin(), sortedPoints.end(),
            [](const CalibrationPoint& a, const CalibrationPoint& b) { return a.value < b.value; });

  if (value <= sortedPoints[0].value) return sortedPoints[0].barSizePercent;
  if (value >= sortedPoints[sortedPoints.size() - 1].value) return sortedPoints[sortedPoints.size() - 1].barSizePercent;

  for (size_t i = 0; i + 1 < sortedPoints.size(); i++) {
    if (value >= sortedPoints[i].value && value <= sortedPoints[i + 1].value) {
      float x1 = sortedPoints[i].value, y1 = sortedPoints[i].barSizePercent;
      float x2 = sortedPoints[i + 1].value, y2 = sortedPoints[i + 1].barSizePercent;
      float percent = (value - x1) / (x2 - x1);
      return y1 + percent * (y2 - y1);
    }
  }
  return 0;
}

// The same interpolation read the other way: a finger has a position, which
// is a percentage of the bar, and what has to be published is the value that
// percentage stands for (docs/2026-09-17-settable-level.md in the designer
// repo, decision 5, mirrored from levelValueFromFill() in
// render-level-indicator.ts).
//
// Sorted by value like the forward direction, so a calibration whose
// percentages fall as the value rises inverts too. A segment whose two
// percentages are equal has no value to give - a range of positions all mean
// the same reading - so its lower value is taken rather than dividing by
// zero.
float ColorScreenRenderer::calibrationValueForPercent(float percent, const std::vector<CalibrationPoint>& points) const {
  if (points.empty()) return 0.0f;
  if (points.size() == 1) return points[0].value;

  std::vector<CalibrationPoint> sortedPoints = points;
  std::sort(sortedPoints.begin(), sortedPoints.end(),
            [](const CalibrationPoint& a, const CalibrationPoint& b) { return a.value < b.value; });

  const CalibrationPoint& first = sortedPoints[0];
  const CalibrationPoint& last = sortedPoints[sortedPoints.size() - 1];
  const bool rising = last.barSizePercent >= first.barSizePercent;
  if (rising ? percent <= first.barSizePercent : percent >= first.barSizePercent) return first.value;
  if (rising ? percent >= last.barSizePercent : percent <= last.barSizePercent) return last.value;

  for (size_t i = 0; i + 1 < sortedPoints.size(); i++) {
    const CalibrationPoint& p1 = sortedPoints[i];
    const CalibrationPoint& p2 = sortedPoints[i + 1];
    const float low = fminf(p1.barSizePercent, p2.barSizePercent);
    const float high = fmaxf(p1.barSizePercent, p2.barSizePercent);
    if (percent < low || percent > high) continue;
    if (p2.barSizePercent == p1.barSizePercent) return p1.value;
    const float ratio = (percent - p1.barSizePercent) / (p2.barSizePercent - p1.barSizePercent);
    return p1.value + ratio * (p2.value - p1.value);
  }
  return first.value;
}

// A level with somewhere to write to is one a finger can set; without one it
// is exactly the read-only bar it has always been (decision 1). Only the bar
// so far - the ring follows (decision 7).
// The value a level shows. What the installation reports - except on a level
// with no setpoint of its own while a finger is asking for a value on it. That
// is the dimmer's pattern in the handbook (designer, mqtt-beispiele.md): one
// value, which a command sets and the state reports back a moment later, so
// while the finger is on it the value IS the finger - fill and handle
// together, as the lamp follows it and as Pekaway's own dashboard shows it.
// Showing the report there split one value in two while answers ran behind
// the finger: the fill jumped about under a steady handle (2026-09-27, in the
// van). A level WITH a setpoint topic is the heater's pattern, where wanted
// and measured are truly different: its fill stays the report, and only the
// marker follows the finger (decision 6c).
String ColorScreenRenderer::shownLevelValue(const ScreenObject& obj) const {
  // Only the control under the finger follows it: a read-only gauge on the
  // same topic keeps showing what is reported.
  if (obj.properties.setpointTopic.isEmpty() && !obj.properties.writeTopic.isEmpty() && !askedValue_.isEmpty() &&
      askedTopic_ == obj.properties.topic) {
    return askedValue_;
  }
  return projectLoader_.getTopicValue(obj.properties.topic);
}

void ColorScreenRenderer::setAskedValue(const String& markerTopic, const String& value) {
  askedTopic_ = markerTopic;
  askedValue_ = value;
}

bool ColorScreenRenderer::markerTopicBounds(int screenIndex, const String& markerTopic, int16_t& x, int16_t& y,
                                            int16_t& w, int16_t& h) const {
  if (!projectLoader_.isLoaded() || markerTopic.isEmpty()) return false;
  const ProjectConfig& project = projectLoader_.getProject();
  const Screen* screenPtr = screenOrPopupAt(project, screenIndex);
  if (!screenPtr) return false;

  bool found = false;
  int16_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  std::function<void(const std::vector<ScreenObject>&, int16_t, int16_t)> walk =
      [&](const std::vector<ScreenObject>& objects, int16_t dx, int16_t dy) {
        for (const auto& o : objects) {
          const String topic = o.properties.setpointTopic.isEmpty() ? o.properties.topic : o.properties.setpointTopic;
          if (objecttypes::isAnyLevel(o.type) && topic == markerTopic) {
            const int16_t ox = dx + o.x;
            const int16_t oy = dy + o.y;
            if (!found) {
              x0 = ox;
              y0 = oy;
              x1 = ox + o.width;
              y1 = oy + o.height;
              found = true;
            } else {
              if (ox < x0) x0 = ox;
              if (oy < y0) y0 = oy;
              if (ox + o.width > x1) x1 = ox + o.width;
              if (oy + o.height > y1) y1 = oy + o.height;
            }
          }
          if (o.type == "switcher") {
            const ScreenObject* panel = getActivePanel(o);
            if (panel != nullptr) walk(panel->children, dx + o.x, dy + o.y);
          }
        }
      };
  walk(screenPtr->objects, 0, 0);
  if (!found) return false;
  x = x0;
  y = y0;
  w = x1 - x0;
  h = y1 - y0;
  return true;
}

void ColorScreenRenderer::clearAskedValue() {
  askedTopic_ = "";
  askedValue_ = "";
}

bool ColorScreenRenderer::isSettableLevel(const ScreenObject& obj) {
  // The type says it now, and only the type: a bar shows a target it
  // cannot move, a slider can move it (docs/2026-09-20-control-split.md
  // in the designer repo).
  return objecttypes::isSettable(obj.type);
}

// Wo ein Finger auf dem Ring liegt, als Prozent seiner eigenen Bahn - die
// Umkehrung von ArcRaster::sweepForPercent(), wortgleich zu
// arcPercentFromPoint() in render-arc-level.ts. Zwoelf Uhr ist oben, die Grade
// laufen im Uhrzeigersinn; ein Punkt in der Luecke unten am Thermostatbogen
// bekommt das naehere Ende, weil das gemeint ist, wenn man ueber das Ende
// einer Skala hinausgreift.
static float arcPercentFromPoint(const ScreenObject& obj, int16_t x, int16_t y, int16_t objX, int16_t objY) {
  const int32_t size = min(obj.width, obj.height) < 1 ? 1 : min(obj.width, obj.height);
  const float cx = objX + size / 2.0f;
  const float cy = objY + size / 2.0f;
  const float dx = x - cx;
  const float dy = y - cy;
  if (dx == 0.0f && dy == 0.0f) return 0.0f;

  const int32_t minA = ((obj.properties.minAngle % 360) + 360) % 360;
  const int32_t maxA = ((obj.properties.maxAngle % 360) + 360) % 360;
  const bool counterClockwise = obj.properties.arcDirection == "ccw";
  const int32_t startDeg = counterClockwise ? maxA : minA;
  int32_t spanDeg = counterClockwise ? ((minA - maxA + 360) % 360) : ((maxA - minA + 360) % 360);
  if (spanDeg == 0) spanDeg = 360;

  float deg = atan2f(dx, -dy) * 180.0f / (float)M_PI;
  if (deg < 0.0f) deg += 360.0f;

  float rel = fmodf(deg - (float)startDeg + 360.0f, 360.0f);
  if (rel > (float)spanDeg) {
    const float pastEnd = rel - (float)spanDeg;
    const float beforeStart = 360.0f - rel;
    rel = pastEnd <= beforeStart ? (float)spanDeg : 0.0f;
  }

  float percent = (rel / (float)spanDeg) * 100.0f;
  if (counterClockwise) percent = 100.0f - percent;
  if (percent < 0.0f) percent = 0.0f;
  if (percent > 100.0f) percent = 100.0f;
  return percent;
}

// The value a finger at this point stands for. For the bar: the same padding
// of 4 and the same four directions renderLevelIndicator() fills with, read
// backwards, so it fills to where the finger is. For the ring: its sector,
// read the same way round. Both snapped to the object's step.
float ColorScreenRenderer::settableLevelValueAt(const ScreenObject& obj, int16_t x, int16_t y,
                                                 int16_t objX, int16_t objY) const {
  if (objecttypes::isArc(obj.type)) {
    std::vector<CalibrationPoint> arcPoints = obj.properties.calibrationPoints;
    if (arcPoints.empty()) {
      arcPoints = {{0.0f, 0.0f}, {100.0f, 100.0f}};
    }
    const float arcValue = calibrationValueForPercent(arcPercentFromPoint(obj, x, y, objX, objY), arcPoints);
    const float arcStep = obj.properties.step;
    if (arcStep <= 0.0f) return arcValue;
    return roundf(arcValue / arcStep) * arcStep;
  }

  const int padding = 4;
  const float innerX = objX + padding;
  const float innerY = objY + padding;
  const float innerWidth = fmaxf(1.0f, obj.width - padding * 2);
  const float innerHeight = fmaxf(1.0f, obj.height - padding * 2);

  float along;
  if (obj.properties.barDirection == "right-to-left") {
    along = (innerX + innerWidth - x) / innerWidth;
  } else if (obj.properties.barDirection == "bottom-to-top") {
    along = (innerY + innerHeight - y) / innerHeight;
  } else if (obj.properties.barDirection == "top-to-bottom") {
    along = (y - innerY) / innerHeight;
  } else {
    along = (x - innerX) / innerWidth;
  }

  float percent = along * 100.0f;
  if (percent < 0.0f) percent = 0.0f;
  if (percent > 100.0f) percent = 100.0f;

  std::vector<CalibrationPoint> points = obj.properties.calibrationPoints;
  if (points.empty()) {
    points = {{0.0f, 0.0f}, {100.0f, 100.0f}};
  }
  const float value = calibrationValueForPercent(percent, points);
  const float step = obj.properties.step;
  if (step <= 0.0f) return value;
  return roundf(value / step) * step;
}

// Sums the glyphs' own advances, which is what the designer's
// BDFFont.measureText() does - so alignment lands on the same pixel. The
// old version drew the string into a zero-sized clip and read u8g2's cursor
// back, because u8g2's own getUTF8Width() disagreed with its drawing by a
// pixel. With one font on both sides there is nothing left to reconcile.
int16_t ColorScreenRenderer::measureTrueTextWidth(const String& text, const BdfFont* font) {
  return font == nullptr ? 0 : font->textWidth(text);
}

void ColorScreenRenderer::drawTextBox(const ScreenObject& obj, const String& displayText, bool drawBorder) {
  const BdfFont* font = fontFor(obj.properties.fontId);

  int fontAscent = getFontAscentById(obj.properties.fontId);
  // Box height for background/border/clip comes from the font's own size
  // (ascent+descent), not obj.height - matches the designer's
  // boundingBoxHeight, not the independently-resizable JSON height field.
  int boxHeight = getFontSizeById(obj.properties.fontId);

  String textColorStr = obj.properties.color.isEmpty() ? obj.properties.textColor : obj.properties.color;
  uint16_t textColor = parseHexColor(textColorStr);

  bool bgTransparent = false;
  uint16_t bgColor = parseHexColor(obj.properties.backgroundColor, &bgTransparent);

  // Background -> border -> text, in that order (text last, always on top)
  // - matches the designer's draw order exactly, see schaltli-eink's
  // drawTextBox() comment for the HIL mismatch this order avoids.
  if (!bgTransparent) {
    canvas_->fillRect(obj.x, obj.y, obj.width, boxHeight, bgColor);
  }

  if (drawBorder) {
    bool borderTransparent = false;
    uint16_t borderColor = parseHexColor(obj.properties.borderColor, &borderTransparent);
    if (!obj.properties.borderColor.isEmpty() && !borderTransparent) {
      canvas_->drawRect(obj.x, obj.y, obj.width, boxHeight, borderColor);
    }
  }

  int16_t baselineY = obj.y + fontAscent;
  int16_t textWidth = measureTrueTextWidth(displayText, font);

  // No +1 here any more. It used to cancel an off-by-one in the designer's
  // glyph loop, which drew each row one pixel right of its bounding box;
  // BdfFont reproduces that shift instead, so the compensation moved with
  // it and every existing project stays on the same pixel. See BdfFont.h.
  int16_t textX = obj.x;
  if (obj.properties.textAlign == "center") {
    textX = obj.x + jsRound((obj.width - textWidth) / 2.0f);
  } else if (obj.properties.textAlign == "right") {
    textX = obj.x + obj.width - textWidth;
  }

  // The box grows upward by whatever the font's ink needs beyond its
  // nominal ascent, so accents on capitals are drawn rather than shaved
  // off. They land on the border, which is what the font says they do.
  const int16_t overhang = inkOverhang(font, displayText, (int16_t)fontAscent);
  canvas_->setClip(obj.x, obj.y - overhang, obj.width, boxHeight + overhang);
  if (font != nullptr) font->drawText(*canvas_, displayText, textX, baselineY, textColor);
  canvas_->clearClip();
}

int16_t ColorScreenRenderer::textOverhangLimit(const ScreenObject& obj) {
  const BdfFont* font = fontFor(obj.properties.fontId);
  if (font == nullptr) return 0;
  const int16_t over = (int16_t)(font->maxInkAscent() - getFontAscentById(obj.properties.fontId));
  return over > 0 ? over : 0;
}

bool ColorScreenRenderer::renderLabel(const ScreenObject& obj) {
  // A device of generation 1.4 reads liveText; `text` beside it is the same
  // text lowered to placeholders, for the devices before (contract 2.6).
  if (!obj.properties.liveText.isEmpty()) {
    const ProjectConfig& project = projectLoader_.getProject();
    placeholders::Separators separators;
    separators.decimal = project.decimalSeparator.c_str();
    separators.thousands = project.thousandsSeparator.c_str();
    const std::string text = livevalue::resolveLiveText(
        obj.properties.liveText.c_str(), obj.properties.liveValues,
        [this](const livevalue::Source& source) { return liveSourceValue(source); }, separators);
    drawTextBox(obj, String(text.c_str()), true);
    return true;
  }
  drawTextBox(obj, resolvePlaceholders(obj.properties.text), true);
  return true;
}

std::optional<std::string> ColorScreenRenderer::liveSourceValue(const livevalue::Source& source) const {
  if (source.ns == "topic") {
    const String topic(source.path.c_str());
    // Never arrived is not the same as empty: only the first is No value yet.
    if (!projectLoader_.hasTopicValue(topic)) return std::nullopt;
    return std::string(projectLoader_.getTopicValue(topic).c_str());
  }
  if (source.ns == "combined") return projectLoader_.getCombinedValue(String(source.path.c_str()));
  if (source.ns == "device") {
    if (source.path == "model") return std::string(DEVICE_MODEL);
    return std::string(deviceId_.c_str());
  }
  if (source.ns == "project" && source.path == "name") return std::string(projectLoader_.getProject().name.c_str());
  return std::nullopt;
}

String ColorScreenRenderer::resolvePlaceholders(const String& text) const {
  const std::string source(text.c_str());
  if (!placeholders::mayContainPlaceholder(source)) return text;
  const ProjectConfig& project = projectLoader_.getProject();
  placeholders::Separators separators;
  separators.decimal = project.decimalSeparator.c_str();
  separators.thousands = project.thousandsSeparator.c_str();
  auto lookup = [this](const placeholders::Reference& reference) -> std::optional<std::string> {
    if (reference.ns == "topic") {
      const String topic(reference.path.c_str());
      // Never arrived is not the same as empty: only the first takes `??`.
      if (!projectLoader_.hasTopicValue(topic)) return std::nullopt;
      return std::string(projectLoader_.getTopicValue(topic).c_str());
    }
    if (reference.ns == "device") {
      if (reference.path == "model") return std::string(DEVICE_MODEL);
      return std::string(deviceId_.c_str());
    }
    // project: is written in by the export and never arrives here.
    return std::nullopt;
  };
  return String(placeholders::resolve(source, lookup, separators).c_str());
}

std::vector<std::string> ColorScreenRenderer::liveTopics(const ScreenObject& obj,
                                                         const std::vector<combined::CombinedTopic>& combinedTopics) {
  std::vector<std::string> out;
  auto add = [&out](const std::string& topic) {
    if (std::find(out.begin(), out.end(), topic) == out.end()) out.push_back(topic);
  };
  // An energy diagram: its nodes' watts and charge, and what their lines
  // read (contract 2.8). The knob subscribes by this; until 2026-10-12 it
  // heard none of them.
  // A topic with a JSON field (`ble/charger/watts#value`) is subscribed
  // without it, as a live value's is.
  if (obj.properties.energy) {
    auto bare = [](const String& t) { return std::string(t.c_str()).substr(0, std::string(t.c_str()).find('#')); };
    for (const EnergyNodeProps& n : obj.properties.energy->nodes) {
      if (!n.topic.isEmpty()) add(bare(n.topic));
      if (!n.chargeTopic.isEmpty()) add(bare(n.chargeTopic));
      for (const EnergyLine& line : n.lines) {
        ScreenObject text;
        text.properties.liveValues = line.liveValues;
        for (const std::string& t : liveTopics(text, combinedTopics)) add(t);
      }
    }
  }
  for (const livevalue::LiveValue& lv : obj.properties.liveValues) {
    if (lv.source.ns == "topic") add(lv.source.path.substr(0, lv.source.path.find('#')));
    if (lv.source.ns == "combined")
      for (const std::string& topic : combined::inputTopics(combinedTopics, lv.source.path)) add(topic);
  }
  return out;
}

bool ColorScreenRenderer::readsTopic(const ScreenObject& obj, const String& topic,
                                     const std::vector<combined::CombinedTopic>& combinedTopics) {
  // An energy diagram reads its nodes' watts and charge, and its lines are
  // live texts (contract 2.8).
  if (obj.properties.energy) {
    for (const EnergyNodeProps& n : obj.properties.energy->nodes) {
      // The arriving topic has no JSON field; a node's may (`…#value`).
      auto reads = [&topic](const String& t) { const int hash = t.indexOf('#'); return topic == (hash < 0 ? t : t.substring(0, hash)); };
      if ((!n.topic.isEmpty() && reads(n.topic)) || (!n.chargeTopic.isEmpty() && reads(n.chargeTopic))) return true;
      for (const EnergyLine& line : n.lines) {
        ScreenObject text;
        text.type = "text";
        text.properties.text = line.text;
        text.properties.liveValues = line.liveValues;
        if (readsTopic(text, topic, combinedTopics)) return true;
      }
    }
    return false;
  }
  if (obj.type == "text") {
    const std::string text(obj.properties.text.c_str());
    if (placeholders::mayContainPlaceholder(text)) {
      for (const std::string& reference : placeholders::referencedTopics(text)) {
        const size_t hash = reference.find('#');
        if (topic == (hash == std::string::npos ? reference : reference.substr(0, hash)).c_str()) return true;
      }
    }
  } else if (obj.properties.liveIconId.isEmpty()) {
    return false;
  }
  for (const std::string& read : liveTopics(obj, combinedTopics))
    if (topic == read.c_str()) return true;
  return false;
}

// The designer's render-text-field.ts, pixel for pixel: the container and
// the line through the same integer rasteriser, the text cut at `pad`.
// What it shows: what was sent until the topic answers (the asked value,
// as a slider's), then the topic's text; the loader has made the write
// topic the read one where there was none.
bool ColorScreenRenderer::renderTextField(const ScreenObject& obj) {
  const textfield::Geometry g = textfield::geometry(obj.width, obj.height);
  const uint16_t field = parseHexColor(obj.properties.fieldColor.isEmpty() ? String("#e7e0ec") : obj.properties.fieldColor);
  const uint16_t line = parseHexColor(obj.properties.lineColor.isEmpty() ? String("#6750a4") : obj.properties.lineColor);
  const uint16_t ink = parseHexColor(obj.properties.textColor.isEmpty() ? String("#1d1b20") : obj.properties.textColor);
  canvas_->fillRoundRect(obj.x, obj.y, obj.width, obj.height, g.radius, field);
  if (obj.width > 2 * g.radius) canvas_->fillRoundRect(obj.x + g.radius, obj.y + g.lineY, obj.width - 2 * g.radius, g.lineHeight, 0, line);

  const String& topic = obj.properties.topic;
  if (topic.isEmpty()) return true;
  String value;
  if (!askedValue_.isEmpty() && askedTopic_ == topic) value = askedValue_;
  else if (projectLoader_.hasTopicValue(topic)) value = projectLoader_.getTopicValue(topic);
  if (value.isEmpty()) return true;

  const BdfFont* font = fontFor(obj.properties.fontId);
  if (font == nullptr) return true;
  const std::string shown =
      textfield::shownText(value.c_str(), obj.properties.hideText, font->hasGlyph(textfield::HIDDEN_CHAR));
  const int ascent = getFontAscentById(obj.properties.fontId);
  const int descent = getFontDescentById(obj.properties.fontId);
  const int16_t baselineY = obj.y + textfield::baseline(obj.height, ascent, descent);
  const int16_t clipW = obj.width - 2 * g.pad;
  if (clipW <= 0) return true;
  canvas_->setClip(obj.x + g.pad, obj.y, clipW, obj.height);
  font->drawText(*canvas_, String(shown.c_str()), obj.x + g.pad, baselineY, ink);
  canvas_->clearClip();
  return true;
}

bool ColorScreenRenderer::renderBox(const ScreenObject& obj) {
  // fillColor, not backgroundColor - box's own fill is a distinct property
  // from backgroundColor (used by label/MqttDataField's text-box
  // background), matching render-box.ts's identical `obj.properties.
  // fillColor` read on the designer side and ObjectProperties.fillColor's
  // own "Color of filled portion" field. Reading backgroundColor here
  // instead (defaulting to opaque "#ffffff" per parseObjectProperties())
  // meant every box with a border rendered a white interior no matter what
  // fillColor actually said, never transparent even when fillColor was
  // never set at all - found 2026-08-10 building the M5 Dial HIL fixture.
  bool fillTransparent = false;
  uint16_t fillColor = parseHexColor(obj.properties.fillColor, &fillTransparent);
  bool strokeTransparent = false;
  uint16_t strokeColor = parseHexColor(obj.properties.strokeColor, &strokeTransparent);
  int strokeWidth = obj.properties.strokeWidth;
  int cornerRadius = obj.properties.cornerRadius;
  if (cornerRadius < 0) cornerRadius = 0;
  bool hasBorder = strokeWidth > 0 && !obj.properties.strokeColor.isEmpty() && !strokeTransparent;

  if (fillTransparent && !hasBorder) return true;

  // The e-paper original's technique (fill the whole box with border color,
  // then paint a smaller inner rect in fill color to "cut out" the
  // interior) only works when fill color is a real paintable color - that
  // firmware's parseColor() never had a transparent concept at all (a
  // monochrome canvas is always definitely black or white). Color mode
  // does have one, and painting a transparent interior by "cutting it out"
  // after already painting over it isn't possible - a transparent-fill
  // bordered box has to draw ONLY the border outline instead, leaving the
  // interior untouched (found live on hardware: a bordered box with a
  // transparent fill rendered fully filled with its own border color,
  // hiding an orange label text drawn on top of it).
  if (fillTransparent) {
    for (int i = 0; i < strokeWidth; i++) {
      int ringRadius = cornerRadius - i;
      if (ringRadius < 0) ringRadius = 0;
      int ringW = obj.width - 2 * i;
      int ringH = obj.height - 2 * i;
      if (ringW <= 0 || ringH <= 0) break;
      if (ringRadius > 0) {
        canvas_->drawRoundRect(obj.x + i, obj.y + i, ringW, ringH, ringRadius, strokeColor);
      } else {
        canvas_->drawRect(obj.x + i, obj.y + i, ringW, ringH, strokeColor);
      }
    }
    return true;
  }

  if (cornerRadius > 0) {
    canvas_->fillRoundRect(obj.x, obj.y, obj.width, obj.height, cornerRadius, hasBorder ? strokeColor : fillColor);
  } else {
    canvas_->fillRect(obj.x, obj.y, obj.width, obj.height, hasBorder ? strokeColor : fillColor);
  }

  if (hasBorder) {
    int innerX = obj.x + strokeWidth;
    int innerY = obj.y + strokeWidth;
    int innerWidth = obj.width - 2 * strokeWidth;
    int innerHeight = obj.height - 2 * strokeWidth;
    int innerRadius = cornerRadius - strokeWidth;
    if (innerRadius < 0) innerRadius = 0;
    if (innerWidth > 0 && innerHeight > 0) {
      if (innerRadius > 0) {
        canvas_->fillRoundRect(innerX, innerY, innerWidth, innerHeight, innerRadius, fillColor);
      } else {
        canvas_->fillRect(innerX, innerY, innerWidth, innerHeight, fillColor);
      }
    }
  }
  return true;
}

// Full parity with the e-paper reference (2026-08-14 port): fillet, thick
// strokes, fixed arrowheads, via the shared renderLineBody/fillThickLine/
// drawArrowhead/shortenForArrow helpers below (see their own comments).
bool ColorScreenRenderer::renderLine(const ScreenObject& obj) {
  // Black when the project sets none, matching render-line.ts.
  uint16_t color = parseHexColor(obj.properties.color.isEmpty() ? "#000000" : obj.properties.color);
  float strokeWidth = (float)obj.properties.strokeWidth;

  std::vector<Point> points = obj.properties.points;
  if (points.size() < 2) {
    points.clear();
    points.push_back({obj.x, obj.y});
    points.push_back({obj.x + obj.width, obj.y + obj.height});
  }

  // strokeStyle other than "solid" (dashed/dotted) isn't supported by this
  // device yet - draw solid regardless, same as the designer's fallback.
  auto drawSegment = [&](int16_t x0, int16_t y0, int16_t x1, int16_t y1) {
    if (strokeWidth > 1) {
      fillThickLine(x0, y0, x1, y1, strokeWidth, color);
    } else {
      canvas_->drawLine(x0, y0, x1, y1, color);
    }
  };

  // Manual, fixed arrowheads (as opposed to MqttDataLine's data-driven
  // ones) - independent start/end flags. The body is drawn shortened at
  // whichever end(s) show one - see shortenForArrow()'s comment for why.
  bool arrowStart = points.size() >= 2 && obj.properties.arrowStart;
  bool arrowEnd = points.size() >= 2 && obj.properties.arrowEnd;

  std::vector<Point> bodyPoints = points;
  if (arrowStart) bodyPoints[0] = shortenForArrow(points[0], points[1], strokeWidth);
  if (arrowEnd) {
    bodyPoints[bodyPoints.size() - 1] = shortenForArrow(points[points.size() - 1], points[points.size() - 2], strokeWidth);
  }
  renderLineBody(bodyPoints, obj.properties.filletRadius, drawSegment);

  if (arrowStart) drawArrowhead(points[0], points[1], strokeWidth, color);
  if (arrowEnd) drawArrowhead(points[points.size() - 1], points[points.size() - 2], strokeWidth, color);

  return true;
}

// MqttDataLine: like renderLine but stroke width is data-driven
// (calibrationPoints maps abs(topic value) to a px width) and arrow
// presence at each end is independently evaluated against the topic's
// signed value (arrowStartOperator/Value, arrowEndOperator/Value), instead
// of renderLine's fixed booleans. Ported near-verbatim from
// schaltli-eink's ScreenRenderer::renderMqttDataLine().
bool ColorScreenRenderer::renderMqttDataLine(const ScreenObject& obj) {
  // Black when the project sets none, matching render-mqtt-data-line.ts.
  uint16_t color = parseHexColor(obj.properties.color.isEmpty() ? "#000000" : obj.properties.color);

  std::vector<Point> points = obj.properties.points;
  if (points.size() < 2) {
    points.clear();
    points.push_back({obj.x, obj.y});
    points.push_back({obj.x + obj.width, obj.y + obj.height});
  }

  String rawValue = projectLoader_.getTopicValue(obj.properties.topic);
  // No value yet, no line - not the thinnest one, which would read as
  // "nothing flowing" (render-mqtt-data-line.ts, the same rule).
  String trimmedValue = rawValue;
  trimmedValue.trim();
  if (trimmedValue.isEmpty()) return true;
  float numericValue = rawValue.toFloat();

  // abs() - magnitude drives width, sign drives direction (the arrow
  // conditions below use rawValue, the signed string, directly). Falls back
  // to the same default calibration the designer's toolbar/property panel
  // pre-populates a new MqttDataLine with, rather than
  // interpolateCalibration()'s own generic empty-vector fallback (returning
  // the raw value unclamped, which for a magnitude like 75 would mean a
  // 75px-wide line).
  std::vector<CalibrationPoint> calibrationPoints = obj.properties.calibrationPoints;
  if (calibrationPoints.empty()) {
    calibrationPoints = {{0.0f, 1.0f}, {100.0f, 6.0f}};
  }
  float strokeWidth = fmaxf(1.0f, roundf(interpolateCalibration(fabsf(numericValue), calibrationPoints)));

  auto drawSegment = [&](int16_t x0, int16_t y0, int16_t x1, int16_t y1) {
    if (strokeWidth > 1) {
      fillThickLine(x0, y0, x1, y1, strokeWidth, color);
    } else {
      canvas_->drawLine(x0, y0, x1, y1, color);
    }
  };

  bool arrowStart = points.size() >= 2 &&
      evaluateVisibilityCondition(rawValue, obj.properties.arrowStartOperator, obj.properties.arrowStartValue);
  bool arrowEnd = points.size() >= 2 &&
      evaluateVisibilityCondition(rawValue, obj.properties.arrowEndOperator, obj.properties.arrowEndValue);

  // Body shortened at whichever end(s) show an arrow - see
  // shortenForArrow()'s comment. Matters more here than on a plain line: a
  // flow line's whole point is a data-driven, potentially very thick stroke.
  std::vector<Point> bodyPoints = points;
  if (arrowStart) bodyPoints[0] = shortenForArrow(points[0], points[1], strokeWidth);
  if (arrowEnd) {
    bodyPoints[bodyPoints.size() - 1] = shortenForArrow(points[points.size() - 1], points[points.size() - 2], strokeWidth);
  }
  renderLineBody(bodyPoints, obj.properties.filletRadius, drawSegment);

  if (arrowStart) drawArrowhead(points[0], points[1], strokeWidth, color);
  if (arrowEnd) drawArrowhead(points[points.size() - 1], points[points.size() - 2], strokeWidth, color);

  return true;
}

void ColorScreenRenderer::renderLineBody(const std::vector<Point>& points, int filletRadius,
                                          const std::function<void(int16_t, int16_t, int16_t, int16_t)>& drawSegment) {
  if (filletRadius <= 0 || points.size() < 3) {
    for (size_t i = 0; i + 1 < points.size(); i++) {
      drawSegment(points[i].x, points[i].y, points[i + 1].x, points[i + 1].y);
    }
    return;
  }

  renderFilletedLine(points, filletRadius, drawSegment);
}

void ColorScreenRenderer::drawArrowhead(const Point& tip, const Point& from, float strokeWidth, uint16_t color) {
  float dx = (float)(tip.x - from.x);
  float dy = (float)(tip.y - from.y);
  float len = hypotf(dx, dy);
  if (len == 0.0f) return;

  float dirX = dx / len;
  float dirY = dy / len;
  float perpX = -dirY;
  float perpY = dirX;

  float arrowLength = fmaxf(6.0f, strokeWidth * 3.0f);
  float arrowHalfWidth = fmaxf(4.0f, strokeWidth * 2.0f);

  float backX = tip.x - dirX * arrowLength;
  float backY = tip.y - dirY * arrowLength;

  int16_t tipX = (int16_t)roundf((float)tip.x);
  int16_t tipY = (int16_t)roundf((float)tip.y);
  int16_t p1x = (int16_t)roundf(backX + perpX * arrowHalfWidth);
  int16_t p1y = (int16_t)roundf(backY + perpY * arrowHalfWidth);
  int16_t p2x = (int16_t)roundf(backX - perpX * arrowHalfWidth);
  int16_t p2y = (int16_t)roundf(backY - perpY * arrowHalfWidth);

  canvas_->fillTriangle(tipX, tipY, p1x, p1y, p2x, p2y, color);
}

Point ColorScreenRenderer::shortenForArrow(const Point& tip, const Point& from, float strokeWidth) {
  float dx = (float)(tip.x - from.x);
  float dy = (float)(tip.y - from.y);
  float len = hypotf(dx, dy);
  if (len == 0.0f) return tip;

  float dirX = dx / len;
  float dirY = dy / len;
  float arrowLength = fmaxf(6.0f, strokeWidth * 3.0f);
  // Clamped so a very short final segment can't shorten past its own start
  // and invert the segment.
  float shortenBy = fminf(arrowLength * 0.25f, len * 0.9f);

  return {(int16_t)roundf(tip.x - dirX * shortenBy), (int16_t)roundf(tip.y - dirY * shortenBy)};
}

// Rounds every interior vertex of a multi-point line via a true circular
// arc between tangent points - see schaltli-eink's ScreenRenderer::
// renderFilletedLine() for the full tangent/arc-center derivation and the
// 2026-07-30 finding on why a Bezier approximation looked visibly wrong at
// sharp angles. Ported verbatim (color-depth-independent geometry).
void ColorScreenRenderer::renderFilletedLine(const std::vector<Point>& points, int filletRadius,
                                              const std::function<void(int16_t, int16_t, int16_t, int16_t)>& drawSegment) {
  const int CURVE_STEPS = 16;
  Point segStart = points[0];

  for (size_t i = 1; i + 1 < points.size(); i++) {
    const Point& prev = points[i - 1];
    const Point& curr = points[i];
    const Point& next = points[i + 1];

    float lenIn = hypotf((float)(curr.x - prev.x), (float)(curr.y - prev.y));
    float lenOut = hypotf((float)(next.x - curr.x), (float)(next.y - curr.y));
    float maxDist = fminf(lenIn / 2.0f, lenOut / 2.0f);

    Point t1, t2;
    float effectiveRadius = 0.0f;
    float centerX = 0.0f, centerY = 0.0f;
    bool haveArc = false;

    if (lenIn <= 0.0f || lenOut <= 0.0f || maxDist <= 0.0f) {
      t1 = curr;
      t2 = curr;
    } else {
      float v1x = (prev.x - curr.x) / lenIn, v1y = (prev.y - curr.y) / lenIn;
      float v2x = (next.x - curr.x) / lenOut, v2y = (next.y - curr.y) / lenOut;
      float dotP = v1x * v2x + v1y * v2y;
      if (dotP > 1.0f) dotP = 1.0f;
      if (dotP < -1.0f) dotP = -1.0f;
      float halfAngle = acosf(dotP) / 2.0f;
      float tanHalfAngle = tanf(halfAngle);
      float sinHalfAngle = sinf(halfAngle);

      float tangentDist = (tanHalfAngle > 1e-4f) ? (filletRadius / tanHalfAngle) : maxDist;
      if (tangentDist > maxDist) tangentDist = maxDist;
      effectiveRadius = tangentDist * tanHalfAngle;

      t1 = {(int16_t)roundf(curr.x + v1x * tangentDist), (int16_t)roundf(curr.y + v1y * tangentDist)};
      t2 = {(int16_t)roundf(curr.x + v2x * tangentDist), (int16_t)roundf(curr.y + v2y * tangentDist)};

      if (effectiveRadius > 0.0f && sinHalfAngle > 1e-4f) {
        float bx = v1x + v2x, by = v1y + v2y;
        float bLen = hypotf(bx, by);
        if (bLen > 1e-4f) {
          bx /= bLen;
          by /= bLen;
          float centerDist = effectiveRadius / sinHalfAngle;
          centerX = curr.x + bx * centerDist;
          centerY = curr.y + by * centerDist;
          haveArc = true;
        }
      }
    }

    drawSegment(segStart.x, segStart.y, t1.x, t1.y);

    if (haveArc) {
      const float kPi = 3.14159265358979323846f;
      float startAngle = atan2f(t1.y - centerY, t1.x - centerX);
      float endAngle = atan2f(t2.y - centerY, t2.x - centerX);
      float delta = endAngle - startAngle;
      while (delta > kPi) delta -= 2.0f * kPi;
      while (delta < -kPi) delta += 2.0f * kPi;

      Point prevCurvePoint = t1;
      for (int s = 1; s <= CURVE_STEPS; s++) {
        float t = (float)s / (float)CURVE_STEPS;
        float angle = startAngle + delta * t;
        float px = centerX + effectiveRadius * cosf(angle);
        float py = centerY + effectiveRadius * sinf(angle);
        Point curvePoint = {(int16_t)roundf(px), (int16_t)roundf(py)};
        drawSegment(prevCurvePoint.x, prevCurvePoint.y, curvePoint.x, curvePoint.y);
        prevCurvePoint = curvePoint;
      }
    }

    segStart = t2;
  }

  const Point& last = points[points.size() - 1];
  drawSegment(segStart.x, segStart.y, last.x, last.y);
}

// Fills a strokeWidth>1 line as a rotated rectangle with butt caps - see
// schaltli-eink's ScreenRenderer::fillThickLine() for the full
// rotated-rectangle inside-test derivation. Ported verbatim except the
// final canvas_->drawPixel() call, which now writes a real RGB565 color
// instead of a 1-bit black/white value - Adafruit_GFX's drawPixel()
// signature is otherwise identical between ClippedCanvas1 and
// ClippedCanvas16.
void ColorScreenRenderer::fillThickLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, float width, uint16_t color) {
  double dx = (double)x1 - (double)x0;
  double dy = (double)y1 - (double)y0;
  double len = sqrt(dx * dx + dy * dy);
  if (len == 0.0) return;  // degenerate (zero-length, butt caps draw nothing)

  double nx = -dy / len;
  double ny = dx / len;
  double hw = (double)width / 2.0;

  // Rectangle corners, in order around the perimeter (A->B->C->D->A):
  // A/B are the two ends of the start cap, C/D the two ends of the end cap.
  double ax = (double)x0 + nx * hw, ay = (double)y0 + ny * hw;
  double bx = (double)x0 - nx * hw, by = (double)y0 - ny * hw;
  double cx = (double)x1 - nx * hw, cy = (double)y1 - ny * hw;
  double dxp = (double)x1 + nx * hw, dyp = (double)y1 + ny * hw;
  double cornersX[4] = {ax, bx, cx, dxp};
  double cornersY[4] = {ay, by, cy, dyp};

  double minXd = cornersX[0], maxXd = cornersX[0];
  double minYd = cornersY[0], maxYd = cornersY[0];
  for (int i = 1; i < 4; i++) {
    if (cornersX[i] < minXd) minXd = cornersX[i];
    if (cornersX[i] > maxXd) maxXd = cornersX[i];
    if (cornersY[i] < minYd) minYd = cornersY[i];
    if (cornersY[i] > maxYd) maxYd = cornersY[i];
  }
  int16_t minX = (int16_t)floor(minXd);
  int16_t maxX = (int16_t)ceil(maxXd);
  int16_t minY = (int16_t)floor(minYd);
  int16_t maxY = (int16_t)ceil(maxYd);

  for (int16_t py = minY; py <= maxY; py++) {
    for (int16_t px = minX; px <= maxX; px++) {
      double testX = (double)px + 0.5;
      double testY = (double)py + 0.5;
      int sign = 0;
      bool inside = true;
      for (int i = 0; i < 4; i++) {
        int j = (i + 1) % 4;
        double edgeX = cornersX[j] - cornersX[i];
        double edgeY = cornersY[j] - cornersY[i];
        double toPointX = testX - cornersX[i];
        double toPointY = testY - cornersY[i];
        double cross = edgeX * toPointY - edgeY * toPointX;
        if (cross != 0.0) {
          int s = (cross > 0.0) ? 1 : -1;
          if (sign == 0) {
            sign = s;
          } else if (s != sign) {
            inside = false;
            break;
          }
        }
      }
      if (inside) {
        canvas_->drawPixel(px, py, color);
      }
    }
  }
}

String ColorScreenRenderer::describeScreen(int screenIndex) {
  if (!projectLoader_.isLoaded()) return "no project loaded\n";
  const ProjectConfig& project = projectLoader_.getProject();
  const Screen* screenPtr = screenOrPopupAt(project, screenIndex);
  if (!screenPtr) return "no such screen\n";
  const Screen& screen = *screenPtr;

  String out = "screen " + String(screenIndex) + " \"" + screen.name + "\", " +
               String((unsigned)screen.objects.size()) + " object(s)\n";
  for (const auto& obj : screen.objects) {
    out += "  " + obj.type + " \"" + obj.id + "\" at " + String(obj.x) + "," + String(obj.y) +
           " " + String(obj.width) + "x" + String(obj.height);
    if (!obj.properties.fontId.isEmpty()) {
      out += "  font " + obj.properties.fontId +
             (fontFor(obj.properties.fontId) == nullptr ? " (NOT LOADED)" : "") +
             " size " + String(getFontSizeById(obj.properties.fontId));
    }
    if (!obj.properties.topic.isEmpty()) {
      const String value = projectLoader_.getTopicValue(obj.properties.topic);
      out += "  topic " + obj.properties.topic + " = \"" + value + "\"";
    }
    if (!obj.children.empty()) out += "  " + String((unsigned)obj.children.size()) + " child(ren)";
    out += "\n";
  }
  return out;
}

bool ColorScreenRenderer::renderMQTTDataField(const ScreenObject& obj) {
  String valueStr = projectLoader_.getTopicValue(obj.properties.topic);

  String trimmedCheck = valueStr;
  trimmedCheck.trim();
  bool isBlank = trimmedCheck.isEmpty();
  if (isBlank) valueStr = "";

  String formattedValue = valueStr;
  if (!isBlank && obj.properties.displayAs == "Formatted Number") {
    formattedValue = formatNumber(valueStr, obj.properties.numberOfDecimals, obj.properties.thousandsSeparator);
  }

  // No value, nothing of it - not a prefix and a unit around an empty number
  // (render-mqtt-field.ts, the same rule).
  String displayText = isBlank ? String("") : obj.properties.prefix + formattedValue + obj.properties.postfix;
  drawTextBox(obj, displayText, true);
  return true;
}

// Same bar/text layout math as schaltli-eink's renderLevelIndicator()
// (padding=4, direction handling, center-align via a single combined
// jsRound division, ascent+descent-based vertical centering) - all of
// that is color-depth-independent. What differs: that firmware's
// "text visible against both the bar and its background" effect is a
// 1-bit pixel-invert trick (draw a tiny GFXcanvas1, then swap every 0<->1
// while blitting) - there's no equivalent "invert" for arbitrary RGB565
// colors. The color equivalent here draws the text twice instead: once
// across the whole object in fillColor, then the bar rect painted solid
// over it (erasing that portion), then the SAME text redrawn a second
// time but clipped to just the bar rect, in bgColor - same visual result
// (fillColor text outside the bar, bgColor text inside it), no invert
// needed.
// One run of text inside a box, pushed against one of its edges and centred in
// its height. Clipped to the box, so a font wider than the reserved column is
// cut off rather than allowed to run into the bar - the column's width is
// guessed from the font size (levelshape::digitWidth), and this is what keeps a
// wrong guess harmless. Mirrors drawLevelTextIn() in the designer's
// render-level-indicator.ts.
// The vertical measure of the object's font - never properties.fontSize, which
// the font picker does not keep in step with the font (designer
// docs/2026-09-19-slider-look.md, decision 13).
LevelFontMetrics ColorScreenRenderer::levelMetrics(const ScreenObject& obj) {
  LevelFontMetrics m;
  const BdfFont* font = fontStore_.get(obj.properties.fontId);
  if (font != nullptr && font->isLoaded()) {
    m.ascent = font->fontAscent();
    m.descent = font->fontDescent();
    m.capHeight = font->capHeight();
    return m;
  }
  const int size = levelshape::fontSize(obj);
  m.ascent = (size * 4) / 5;
  m.descent = size - m.ascent > 0 ? size - m.ascent : 1;
  m.capHeight = (size * 7) / 10;
  return m;
}

// One piece of text, its left end at `x` and standing on `baseline`, clipped to
// `clip`.
void ColorScreenRenderer::drawLevelText(const String& text, const LevelRect& clip, int x, int baseline,
                                        const String& fontId, uint16_t color) {
  if (text.isEmpty() || !clip.valid()) return;
  const BdfFont* font = fontFor(fontId);
  if (font == nullptr) return;
  canvas_->setClip(clip.x, clip.y, clip.w, clip.h);
  font->drawText(*canvas_, text, (int16_t)x, (int16_t)baseline, color);
  canvas_->clearClip();
}

String ColorScreenRenderer::levelSubFontId(const ScreenObject& obj) {
  const ProjectConfig& project = projectLoader_.getProject();
  if (project.fonts.empty()) return obj.properties.fontId;

  auto family = [](const String& id) {
    int end = id.length();
    while (end > 0 && isdigit((unsigned char)id[end - 1])) end--;
    return id.substring(0, end);
  };

  const String ownFamily = family(obj.properties.fontId);
  // Two thirds of the object's own line, in the unit a project font's size is
  // in (ascent plus descent). It was two thirds of fontSize until 2026-09-19,
  // which nothing keeps in step with the font.
  const int wanted = (levelshape::lineHeight(levelMetrics(obj)) * 2) / 3;

  const Font* bestInFamily = nullptr;
  const Font* bestAny = nullptr;
  for (const Font& font : project.fonts) {
    if (font.size > wanted) continue;
    if (bestAny == nullptr || font.size > bestAny->size) bestAny = &font;
    if (family(font.id) != ownFamily) continue;
    if (bestInFamily == nullptr || font.size > bestInFamily->size) bestInFamily = &font;
  }
  if (bestInFamily != nullptr) return bestInFamily->id;
  if (bestAny != nullptr) return bestAny->id;
  return obj.properties.fontId;
}

#if PILL_ANTIALIASED
// Sixteen sub-samples a pixel, mixed in 5/6/5 - the arc's rules exactly.
// See the declaration in ColorScreenRenderer.h for what the order of the runs
// means and why they all arrive in one call.
void ColorScreenRenderer::paintPills(const std::vector<PaintedPill>& painted, uint16_t mixInto,
                                     const PillShading* shading) {
  std::vector<PillRaster::Band> bands;
  std::vector<ArcRaster::Rgb565> colours;
  std::vector<uint16_t> packed;
  // Not std::vector<bool>: that one is a bit field with a proxy reference, and
  // this is read once per band per pixel.
  std::vector<uint8_t> paints;
  std::vector<uint8_t> graded;
  graded.reserve(painted.size());
  bands.reserve(painted.size());
  colours.reserve(painted.size());
  packed.reserve(painted.size());
  paints.reserve(painted.size());
  for (size_t i = 0; i < painted.size(); i++) {
    const PaintedPill& p = painted[i];
    if (p.band.w <= 0 || p.band.h <= 0) continue;
    bands.push_back(p.band);
    colours.push_back(ArcRaster::toRgb565(p.colour));
    packed.push_back(p.colour);
    paints.push_back(p.paints ? 1 : 0);
    graded.push_back(p.graded && shading != nullptr ? 1 : 0);
  }
  if (bands.empty()) return;

  PillRaster::Box bounds = PillRaster::bandsBounds(bands);
  const bool glowing = shading != nullptr && shading->glowPx > 0;
  if (glowing) {
    // Out to where the glow may reach, and no further than the object -
    // paint-pills.ts, bound for bound.
    const int32_t x0 = shading->boxX > bounds.x - 8 ? shading->boxX : bounds.x - 8;
    const int32_t y0 = shading->boxY > bounds.y - 8 ? shading->boxY : bounds.y - 8;
    const int32_t bx1 = bounds.x + bounds.w + 8, by1 = bounds.y + bounds.h + 8;
    const int32_t x1 = shading->boxX + shading->boxW < bx1 ? shading->boxX + shading->boxW : bx1;
    const int32_t y1 = shading->boxY + shading->boxH < by1 ? shading->boxY + shading->boxH : by1;
    PillRaster::Box grown;
    grown.x = x0 < bounds.x ? x0 : bounds.x;
    grown.y = y0 < bounds.y ? y0 : bounds.y;
    grown.w = (x1 > grown.x ? x1 : grown.x) - grown.x;
    grown.h = (y1 > grown.y ? y1 : grown.y) - grown.y;
    bounds = grown;
  }
  if (bounds.w <= 0 || bounds.h <= 0) return;

  // A pixel's step along the track, and its colour in the gradient.
  auto stepAlong = [&](int32_t along) {
    int32_t j = along - shading->trackStart;
    if (j > shading->trackLength - 1) j = shading->trackLength - 1;
    if (j < 0) j = 0;
    return shading->fromEnd ? shading->trackLength - 1 - j : j;
  };
  // One colour per pixel of the track, worked out once: three divisions per
  // pixel of fill were a sixth of a slider's render (4.3B, 2026-09-28).
  std::vector<ArcRaster::Rgb565> trackColours;
  if (shading != nullptr) {
    trackColours.resize(shading->trackLength);
    for (int32_t j = 0; j < shading->trackLength; j++) {
      trackColours[j] = levelglow::gradient565(shading->from, shading->to, stepAlong(shading->trackStart + j),
                                              shading->trackLength);
    }
  }
  auto gradedAt = [&](int32_t along) {
    int32_t j = along - shading->trackStart;
    if (j > shading->trackLength - 1) j = shading->trackLength - 1;
    if (j < 0) j = 0;
    return trackColours[j];
  };

  // Only the clipped cut-through, for the reason renderArcLevel gives at
  // length: pixelBands() samples every pixel sixteen times against every run,
  // so the walk is the whole cost, and a partial redraw should pay only for
  // what really changes. Without a clip, the runs' own bounding box.
  int32_t pxFrom = bounds.x, pxTo = bounds.x + bounds.w;
  int32_t pyFrom = bounds.y, pyTo = bounds.y + bounds.h;
  if (canvas_->clipActive()) {
    const int32_t cx0 = (int32_t)canvas_->clipX();
    const int32_t cy0 = (int32_t)canvas_->clipY();
    const int32_t cx1 = cx0 + (int32_t)canvas_->clipW();
    const int32_t cy1 = cy0 + (int32_t)canvas_->clipH();
    if (cx0 > pxFrom) pxFrom = cx0;
    if (cy0 > pyFrom) pyFrom = cy0;
    if (cx1 < pxTo) pxTo = cx1;
    if (cy1 < pyTo) pyTo = cy1;
  }

  const ArcRaster::Rgb565 mix = ArcRaster::toRgb565(mixInto);
  std::vector<uint8_t> counts(bands.size(), 0);

  for (int32_t py = pyFrom; py < pyTo; py++) {
    for (int32_t px = pxFrom; px < pxTo; px++) {
      PillRaster::pixelBandsQuick(bands, px, py, counts.data());

      // The same arithmetic ArcRaster::blendBands does, over a list rather
      // than over three fixed pairs: a ring has exactly three bands and never
      // will have a fourth, while a pill control has as many runs as it has
      // parts. Accumulated in one pass and rounded once - an unrounded divide
      // would bias every anti-aliased edge half a step darker on one side, and
      // rounding per run would bias it several times over.
      int32_t r = 0, g = 0, b = 0;
      int32_t covered = 0;
      int32_t claimed = 0;
      int inked = 0;
      size_t only = 0;
      ArcRaster::Rgb565 onlyColour;
      for (size_t i = 0; i < bands.size(); i++) {
        const uint8_t count = counts[i];
        claimed += count;
        // A run that paints nothing still claims its sub-samples - that is
        // what makes a hole a hole - it simply contributes none of them to the
        // mixture, so they fall to the background below like any other
        // uncovered sub-sample.
        if (count == 0 || paints[i] == 0) continue;
        const ArcRaster::Rgb565 colour = graded[i] ? gradedAt(shading->vertical ? py : px) : colours[i];
        r += (int32_t)colour.r * count;
        g += (int32_t)colour.g * count;
        b += (int32_t)colour.b * count;
        covered += count;
        inked++;
        only = i;
        onlyColour = colour;
      }

      // The glow, mixed into what already stands there - drawn back to front,
      // whatever lies under the control is on the canvas already.
      ArcRaster::Rgb565 mixHere = mix;
      int32_t level = 0;
      if (glowing && claimed < PillRaster::COVERAGE_MAX) {
        const int32_t S = ArcRaster::SUBPIXEL_SCALE;
        const int32_t x = px * S + S / 2, y = py * S + S / 2;
        const int64_t d2 =
            shading->vertical
                ? levelglow::segmentDistance2(x, y, shading->cross, shading->a, shading->cross, shading->b)
                : levelglow::segmentDistance2(x, y, shading->a, shading->cross, shading->b, shading->cross);
        level = levelglow::levelFromDistance2(d2, shading->half);
        if (level > shading->glowPx) level = 0;
      }
      if (level > 0) {
        const int32_t along = shading->vertical ? py : px;
        const int32_t clamped = shading->fromEnd ? (along > shading->edge ? along : shading->edge)
                                                 : (along < shading->edge - 1 ? along : shading->edge - 1);
        const ArcRaster::Rgb565 under = ArcRaster::toRgb565(canvas_->getPixel((int16_t)px, (int16_t)py));
        mixHere = levelglow::blend565(under, gradedAt(clamped), levelglow::ALPHA[level]);
        if (covered == 0) {
          canvas_->drawPixel((int16_t)px, (int16_t)py, ArcRaster::packRgb565(mixHere));
          continue;
        }
      }

      // Nothing of the control here. The pixel is left alone rather than
      // painted with the background: a pill has no square of its own to own,
      // so whatever is behind it - a box, an image - still shows through.
      if (covered == 0) continue;

      // A pixel that one run owns outright keeps that run's colour exactly.
      // On the designer's 8-bit canvas and on Android's ARGB_8888 bitmap that
      // is the point of the rule: the body of a bar would otherwise come back
      // a step off through 5/6/5 and not match the box beside it. Here the
      // framebuffer already IS 5/6/5, so the two are provably the same value
      // ((16c + 8) / 16 == c). It stays because the rule is shared, and
      // because it skips the arithmetic on most of the control's pixels.
      if (inked == 1 && covered == PillRaster::COVERAGE_MAX) {
        canvas_->drawPixel((int16_t)px, (int16_t)py,
                           graded[only] ? ArcRaster::packRgb565(onlyColour) : packed[only]);
        continue;
      }

      const int32_t rest = PillRaster::COVERAGE_MAX - covered;
      r += (int32_t)mixHere.r * rest;
      g += (int32_t)mixHere.g * rest;
      b += (int32_t)mixHere.b * rest;
      // All operands are non-negative, so a plain divide floors - the same
      // thing Math.floor does on the designer's side.
      const int32_t half = PillRaster::COVERAGE_MAX / 2;
      ArcRaster::Rgb565 out;
      out.r = (uint8_t)((r + half) / PillRaster::COVERAGE_MAX);
      out.g = (uint8_t)((g + half) / PillRaster::COVERAGE_MAX);
      out.b = (uint8_t)((b + half) / PillRaster::COVERAGE_MAX);
      canvas_->drawPixel((int16_t)px, (int16_t)py, ArcRaster::packRgb565(out));
    }
  }
}
#else
// Below 24 bit nothing is softened, so nothing calls this. Defined all the
// same, because a declaration without a definition is a link error waiting for
// the first caller that forgets the gate.
void ColorScreenRenderer::paintPills(const std::vector<PaintedPill>&, uint16_t, const PillShading*) {}
#endif  // PILL_ANTIALIASED

bool ColorScreenRenderer::renderLevelIndicator(const ScreenObject& obj) {
  const String valueStr = shownLevelValue(obj);

  // One colour: the bar's. The unfilled track is mixed from it and the screen
  // behind it, and the object has no background, border or track colour of its
  // own any more (designer docs/2026-09-19-slider-look.md, decision 12).
  const String fillHex = materialcolors::applyDepth(
      obj.properties.fillColor.isEmpty() ? String("#4CAF50") : obj.properties.fillColor);
  // The track's colour and edge are the theme's where the export carries them
  // (levelshape::trackPaint).
  const levelshape::LevelTrackPaint look =
      levelshape::trackPaint(obj.properties.trackColor, obj.properties.trackEdgeColor, fillHex, currentScreenBgHex_);
  const uint16_t fillColor = parseHexColor(fillHex);
  const uint16_t trackColor = parseHexColor(look.track);
  const uint16_t edgeColor = parseHexColor(look.edge);
  // A handle a finger can move keeps the bar's own colour - handle and filled
  // track are one object that the gap separates - and one that only reports the
  // installation's target takes the track's colour and steps back. The user, on
  // the picture: "seine dimmed farbe sagt mir dass ich ihn nicht bewegen kann".
  // Where the track is only an outline there is no quiet colour to step back
  // into, so every handle there stays solid. handleColourFor() in the designer's
  // render-level-indicator.ts, and the same three lines renderArcLevel already
  // carries - the ring and the bar have to say this the same way or the two
  // sides of one control contradict each other.
  //
  // "Settable" here is a non-blank writeTopic and nothing else. NOT
  // objecttypes::isSettable, which answers the other question - which objects
  // accept a finger at all - and stays for that.
  const uint16_t handleColor =
      parseHexColor((!obj.properties.writeTopic.isEmpty() || look.framed) ? fillHex : look.track);
  const uint16_t textColor =
      parseHexColor(obj.properties.textColor.isEmpty() ? String("#000000") : obj.properties.textColor);
  // A bar's pointer is in the text's colour - the fill's belongs to what a
  // finger can move - quantised the way the designer quantises it.
  const uint16_t pointerColor = parseHexColor(materialcolors::applyDepth(
      obj.properties.textColor.isEmpty() ? String("#000000") : obj.properties.textColor));

  const LevelFontMetrics metrics = levelMetrics(obj);
  const LevelLayout lay = levelshape::layout(obj, metrics);
  const bool vertical = levelshape::isVertical(obj);

#if !PILL_ANTIALIASED
  // The whole-pixel path, unchanged since before the rasterizer existed and
  // kept line for line: below 24 bit there is nothing to mix into, so this is
  // still the picture (PillRaster.h says why that is a decision).
  //
  // One run of track or fill, painted flush at the ends that are not the
  // track's own, and - where the mixed colour cannot be told from the
  // background - as an outline in the bar's colour with the background cut back
  // into it. Cut rather than stroked: a stroke of one pixel breaks up on a
  // panel that has no half tones.
  auto drawRun = [&](const LevelSegment& seg, uint16_t colour) {
    if (!seg.valid()) return;
    canvas_->fillRoundRect(seg.x, seg.y, seg.w, seg.h, seg.r, colour);
    int r = seg.r;
    const int half = (seg.w < seg.h ? seg.w : seg.h) / 2;
    if (r > half) r = half;
    if (r <= 0) return;
    if (!seg.roundStart) {
      if (vertical) canvas_->fillRect(seg.x, seg.y, seg.w, r, colour);
      else canvas_->fillRect(seg.x, seg.y, r, seg.h, colour);
    }
    if (!seg.roundEnd) {
      if (vertical) canvas_->fillRect(seg.x, seg.y + seg.h - r, seg.w, r, colour);
      else canvas_->fillRect(seg.x + seg.w - r, seg.y, r, seg.h, colour);
    }
  };
  auto drawTrackRun = [&](const LevelSegment& seg) {
    if (!look.framed) {
      drawRun(seg, trackColor);
      return;
    }
    drawRun(seg, edgeColor);
    LevelSegment inner;
    if (levelshape::frameInner(seg, vertical, inner, look.edgeWidth)) drawRun(inner, trackColor);
  };
#endif

  // Everything this bar is made of, described rather than painted, and handed
  // to one rasterizer (PillRaster.h and paintPills above).
  //
  // The order IS the picture: a sub-sample belongs to the first run that
  // contains it and to no other. Painting the handle over the track - which is
  // what the whole-pixel path above still does, because it has nothing to mix
  // - would leave a ring of track colour around it where the two meet, and the
  // same along the value's edge, which is the one place on a bar anybody looks
  // at.
  // The pointer, when there is one, as a triangle band: up under a horizontal
  // bar, left beside a vertical one.
  auto pointerBand = [&](const LevelRect& r, bool pointsLeft) {
    PillRaster::Band band;
    band.x = r.x;
    band.y = r.y;
    band.w = r.w;
    band.h = r.h;
    band.tip = pointsLeft ? PillRaster::TIP_LEFT : PillRaster::TIP_UP;
    return band;
  };

  // The theme's look where the fill is its accent (designer lib/themes.ts):
  // the fill runs from fillColor to fillEndColor along the track, one step per
  // pixel of the track, and a weak glow lies around the filled part. Only where
  // the two ends differ - a gradient from a colour to itself is a flat fill.
  // `glowPercent` is the fill's, or negative for no glow.
  const String endHex =
      obj.properties.fillEndColor.isEmpty() ? String() : materialcolors::applyDepth(obj.properties.fillEndColor);
  const bool gradient = PILL_ANTIALIASED && !endHex.isEmpty() && !look.framed && !endHex.equalsIgnoreCase(fillHex);
  auto shadingFor = [&](float glowPercent) {
    PillShading s;
    s.from = ArcRaster::toRgb565(fillColor);
    s.to = ArcRaster::toRgb565(parseHexColor(endHex));
    s.vertical = vertical;
    s.fromEnd = levelshape::fillsFromEnd(obj);
    s.trackStart = vertical ? lay.track.y : lay.track.x;
    const int32_t length = vertical ? lay.track.h : lay.track.w;
    s.trackLength = length > 1 ? length : 1;
    const int32_t glowPx = levelglow::glowPx(obj.properties.glow);
    if (glowPx > 0 && glowPercent > 0.0f) {
      // Around the filled part: a capsule on the track's centre line from the
      // track's own end to the fill's edge, half the thickness wide.
      const int32_t S = ArcRaster::SUBPIXEL_SCALE;
      s.glowPx = glowPx;
      s.edge = levelshape::edgeFor(lay.track, vertical, s.fromEnd, glowPercent);
      s.half = (vertical ? lay.track.w : lay.track.h) * S / 2;
      s.cross = vertical ? lay.track.x * S + lay.track.w * S / 2 : lay.track.y * S + lay.track.h * S / 2;
      const int32_t lo = (s.fromEnd ? s.edge : s.trackStart) * S;
      const int32_t hi = (s.fromEnd ? s.trackStart + s.trackLength : s.edge) * S;
      const int32_t p = lo + s.half, q = hi - s.half;
      s.a = p < q ? p : q;
      s.b = p < q ? q : p;
      s.boxX = obj.x;
      s.boxY = obj.y;
      s.boxW = obj.width;
      s.boxH = obj.height;
    }
    return s;
  };

  auto paintBar = [&](const std::vector<LevelSegment>& runs, const LevelRect* handle,
                      const PillRaster::Band* pointer, float glowPercent) {
#if PILL_ANTIALIASED
    const PillShading shading = shadingFor(glowPercent);
    // A run's two ends: rounded where the track itself ends, square where
    // something cut it - the fill's edge, or the handle's gap. That is
    // Material's 2 dp inner corner taken to its limit (LevelShape.h).
    auto bandOf = [&](const LevelSegment& seg) {
      PillRaster::Band band;
      band.x = seg.x;
      band.y = seg.y;
      band.w = seg.w;
      band.h = seg.h;
      band.rLow = seg.roundStart ? seg.r : 0;
      band.rHigh = seg.roundEnd ? seg.r : 0;
      band.vertical = vertical;
      return band;
    };
    auto pillOf = [](const PillRaster::Band& band, uint16_t colour) {
      PaintedPill out;
      out.band = band;
      out.colour = colour;
      out.paints = true;
      return out;
    };

    std::vector<PaintedPill> painted;
    if (pointer != nullptr) painted.push_back(pillOf(*pointer, pointerColor));
    if (handle != nullptr && handle->valid()) {
      PillRaster::Band band;
      band.x = handle->x;
      band.y = handle->y;
      band.w = handle->w;
      band.h = handle->h;
      band.rLow = handle->r;
      band.rHigh = handle->r;
      // It lies ACROSS the bar, so its own long axis is the other one.
      band.vertical = !vertical;
      uint16_t colour = handleColor;
      // Where the fill runs from one colour to another, a handle a finger
      // moves takes the colour of the fill where it stands.
      if (gradient && isSettableLevel(obj)) {
        const int32_t along = vertical ? handle->y + handle->h / 2 : handle->x + handle->w / 2;
        int32_t j = along - shading.trackStart;
        if (j > shading.trackLength - 1) j = shading.trackLength - 1;
        if (j < 0) j = 0;
        if (shading.fromEnd) j = shading.trackLength - 1 - j;
        colour = ArcRaster::packRgb565(levelglow::gradient565(shading.from, shading.to, j, shading.trackLength));
      }
      painted.push_back(pillOf(band, colour));
    }
    for (const LevelSegment& seg : runs) {
      if (seg.isFill) {
        PaintedPill fill = pillOf(bandOf(seg), fillColor);
        fill.graded = gradient;
        painted.push_back(fill);
        continue;
      }
      // The unfilled track: a body in its mixed colour, or - where that colour
      // cannot be told from the background - an outline in the bar's own
      // colour. The outline is the run's outer pixel with its inside taken
      // back, not a ring behind it, so it ends straight where a run is cut
      // instead of in a rounded cap. The inside comes first, being the one
      // that wins the pixels it covers.
      if (!look.framed) {
        painted.push_back(pillOf(bandOf(seg), trackColor));
        continue;
      }
      LevelSegment inner;
      if (levelshape::frameInner(seg, vertical, inner, look.edgeWidth))
        painted.push_back(pillOf(bandOf(inner), trackColor));
      painted.push_back(pillOf(bandOf(seg), edgeColor));
    }
    paintPills(painted, parseHexColor(materialcolors::applyDepth(currentScreenBgHex_)),
               gradient ? &shading : nullptr);
#else
    (void)glowPercent;
    for (const LevelSegment& seg : runs) {
      if (seg.isFill) drawRun(seg, fillColor);
      else drawTrackRun(seg);
    }
    if (handle != nullptr && handle->valid()) {
      canvas_->fillRoundRect(handle->x, handle->y, handle->w, handle->h, handle->r, handleColor);
    }
    if (pointer != nullptr) {
      for (int32_t py = pointer->y; py < pointer->y + pointer->h; py++) {
        for (int32_t px = pointer->x; px < pointer->x + pointer->w; px++) {
          if (PillRaster::tipHoldsPixel(*pointer, px, py)) canvas_->drawPixel(px, py, pointerColor);
        }
      }
    }
#endif
  };

  String trimmedValue = valueStr;
  trimmedValue.trim();
  if (trimmedValue.isEmpty()) {
    // Nothing has arrived: the empty track, and neither fill nor number. An
    // empty fill would claim an empty tank.
    std::vector<LevelSegment> empty;
    empty.push_back(levelshape::emptyTrack(lay));
    paintBar(empty, nullptr, nullptr, -1.0f);
    return true;
  }

  float fillPercentF = interpolateCalibration(valueStr.toFloat(), obj.properties.calibrationPoints);
  if (fillPercentF < 0.0f) fillPercentF = 0.0f;
  if (fillPercentF > 100.0f) fillPercentF = 100.0f;

  const String markerTopic =
      obj.properties.setpointTopic.isEmpty() ? obj.properties.topic : obj.properties.setpointTopic;
  // A bar has no handle, only a pointer, and it points at a target the
  // installation reports - never at a request, which is a finger's, nor at its
  // own value. Whatever write topic it still carries makes no difference
  // (2026-09-28).
  const bool settable = isSettableLevel(obj);
  String rawMarker;
  if (!settable && obj.properties.setpointTopic.isEmpty()) {
    // no marker
  } else if (!askedTopic_.isEmpty() && askedTopic_ == markerTopic && !askedValue_.isEmpty()) {
    rawMarker = askedValue_;
  } else if (!obj.properties.setpointTopic.isEmpty()) {
    rawMarker = projectLoader_.getTopicValue(obj.properties.setpointTopic);
  } else if (settable && !obj.properties.writeTopic.isEmpty()) {
    // Only a slider rests a handle on its value - a bar never, whatever
    // topics it still carries.
    rawMarker = valueStr;
  }
  String trimmedMarker = rawMarker;
  trimmedMarker.trim();

  bool hasMarker = !trimmedMarker.isEmpty();
  float markerPercent = 0.0f;
  if (hasMarker) {
    markerPercent = interpolateCalibration(rawMarker.toFloat(), obj.properties.calibrationPoints);
    if (markerPercent < 0.0f) markerPercent = 0.0f;
    if (markerPercent > 100.0f) markerPercent = 100.0f;
  }

  // Only a slider has a handle. A bar shows the same target with a pointer
  // beside the track instead, which leaves the track whole.
  LevelRect handle;
  const bool hasHandle = hasMarker && lay.track.valid() && settable;
  if (hasHandle) handle = levelshape::handleRect(obj, lay, markerPercent);
  LevelRect pointerBox;
  bool pointsLeft = false;
  const bool hasPointer = hasMarker && lay.track.valid() && !settable &&
                          levelshape::pointerRect(obj, lay, markerPercent, pointerBox, pointsLeft);
  const PillRaster::Band pointer = hasPointer ? pointerBand(pointerBox, pointsLeft) : PillRaster::Band();

  std::vector<LevelSegment> runs;
  levelshape::segments(obj, lay, fillPercentF, hasHandle ? &handle : nullptr, runs);
  paintBar(runs, hasHandle ? &handle : nullptr, hasPointer ? &pointer : nullptr, fillPercentF);

  const bool asPercent = obj.properties.displayValue == "percentage";
  const String measured = asPercent ? String((int)(fillPercentF + 0.5f)) + "%" : valueStr;
  const String commanded =
      hasMarker ? (asPercent ? String((int)(markerPercent + 0.5f)) + "%" : rawMarker) : measured;

  // The number in its own column at the far end of the bar.
  if (lay.hasValue && lay.value.valid() && levelshape::showsNumber(obj)) {
    const BdfFont* ownFont = fontFor(obj.properties.fontId);
    if (ownFont != nullptr) {
      const int width = measureTrueTextWidth(commanded, ownFont);
      drawLevelText(commanded, lay.value, lay.value.x + lay.value.w - width, lay.baseline,
                    obj.properties.fontId, textColor);
    }
  }

  return true;
}

bool ColorScreenRenderer::evaluateCondition(float value, const String& op, float threshold) const {
  if (op == ">") return value > threshold;
  if (op == ">=") return value >= threshold;
  if (op == "<") return value < threshold;
  if (op == "<=") return value <= threshold;
  // MQTTIconField's valueIconPairs use a single "=" (see the designer's
  // mqtt-icon-field-properties.tsx) - both spellings accepted, matching
  // schaltli-eink's identical fix for this exact confusion.
  if (op == "==" || op == "=") return value == threshold;
  if (op == "!=") return value != threshold;
  return false;
}

bool ColorScreenRenderer::evaluateVisibilityCondition(const String& actualValue, const String& op, const String& comparisonValue) const {
  if (op == "==" || op == "!=") {
    String a = actualValue;
    String b = comparisonValue;
    a.trim();
    b.trim();
    bool matches = (a == b);
    return op == "==" ? matches : !matches;
  }
  // Numeric operators: String::toFloat() returns 0.0 for a non-numeric
  // string (not NaN), on both sides here, matching Arduino's own
  // conversion semantics exactly.
  return evaluateCondition(actualValue.toFloat(), op, comparisonValue.toFloat());
}

String ColorScreenRenderer::getIconPathForValue(const ScreenObject& obj, const String& valueStr) const {
  float value = valueStr.toFloat();
  for (const auto& pair : obj.properties.valueIconPairs) {
    if (evaluateCondition(value, pair.comparisonOperator, pair.value)) {
      return pair.path;
    }
  }
  return "";
}

// Between the commanded number and the measured one under it, exactly the
// designer's ARC_SUB_GAP.
static const int16_t kArcSubGap = 2;

// The band's thickness and how far the ring sits inside the object's edge -
// both in one place, because arcLevelDirtyRect() has to arrive at the same two
// numbers and a second copy of them is precisely the copy-paste trap that has
// caught this flow once already (see that function's own note).
static int32_t arcThicknessFor(const ScreenObject& obj, int32_t size) {
  // 0 means the author set none, and then both shapes draw the one default
  // they now share (levelshape::kDefaultThickness, 16 since 2026-09-22).
  int32_t thickness =
      obj.properties.arcThickness > 0 ? obj.properties.arcThickness : levelshape::kDefaultThickness;
  if (thickness < 1) thickness = 1;
  if (thickness > size / 2) thickness = size / 2;
  return thickness;
}

static int32_t arcInsetFor(const ScreenObject& obj, int32_t size, int32_t thickness) {
  // (11/4 t - t) / 2, rounded up: half the handle's overhang. Written as
  // (7t + 7) / 8 because C++'s integer division floors for these and the
  // designer's Math.ceil does not. And room for the glow a theme lays around
  // the fill (LevelGlow.h), which stays inside the object's box like
  // everything else it draws - arcInset in the designer.
  const bool canHaveHandle = !obj.properties.writeTopic.isEmpty() || !obj.properties.setpointTopic.isEmpty();
  const int32_t handleRoom = canHaveHandle ? (thickness * 7 + 7) / 8 : 0;
  const int32_t glowRoom = levelglow::glowPx(obj.properties.glow);
  const int32_t wanted = handleRoom > glowRoom ? handleRoom : glowRoom;
  if (wanted == 0) return 0;
  const int32_t room = size / 2 - thickness - 1;
  const int32_t inset = wanted < room ? wanted : room;
  return inset > 0 ? inset : 0;
}

// The round counterpart of renderLevelIndicator. Same value mapping, same
// text centring - the two are siblings, and matching the bar is what keeps
// this port honest.
//
// Every pixel is computed rather than drawn with a primitive, because there
// is no arc primitive on this canvas (GFXcanvas16 offers 90-degree circle
// helpers and nothing finer) and because an arc drawn by a primitive would
// have hard edges. See ArcRaster.h for why the arithmetic is shaped the way
// it is.
bool ColorScreenRenderer::renderArcLevel(const ScreenObject& obj, uint16_t mixInto) {
  String valueStr = shownLevelValue(obj);
  // No value yet: the track alone - no fill (not even what the calibration
  // makes of 0), no handle, no number (render-arc-level.ts, the same rule).
  String trimmedValue = valueStr;
  trimmedValue.trim();
  const bool noValue = trimmedValue.isEmpty();
  float value = noValue ? 0.0f : valueStr.toFloat();
  float fillPercent = noValue ? 0.0f : interpolateCalibration(value, obj.properties.calibrationPoints);
  if (fillPercent < 0.0f) fillPercent = 0.0f;
  if (fillPercent > 100.0f) fillPercent = 100.0f;

  // Only a ring that CAN have a handle ever draws one - a write topic or a
  // setpoint topic, the same two things the inset below reserves room for. An
  // asked value is keyed by TOPIC, so without that guard a read-only gauge
  // sharing a dimmer's topic would draw a handle it had reserved no room for
  // (render-arc-level.ts asks exactly this question, and so does arcInset).
  const bool canHaveHandle =
      !obj.properties.writeTopic.isEmpty() || !obj.properties.setpointTopic.isEmpty();

  // What was asked for, beside what is measured: a request a finger just made
  // here first, otherwise a setpoint topic's reported value. Am Topic, nicht am
  // Objekt: zwei Regler auf einem Dimmer sind ein Wert, und beide zeigen, was
  // von ihm verlangt wurde (Entscheidung 6c).
  String rawMarker;
  if (!noValue && canHaveHandle) {
    const String markerTopic =
        obj.properties.setpointTopic.isEmpty() ? obj.properties.topic : obj.properties.setpointTopic;
    if (!isSettableLevel(obj) && obj.properties.setpointTopic.isEmpty()) {
      // A gauge points only at a target the installation reports, as a bar
      // does.
    } else if (!askedTopic_.isEmpty() && askedTopic_ == markerTopic && !askedValue_.isEmpty()) {
      rawMarker = askedValue_;
    } else if (!obj.properties.setpointTopic.isEmpty()) {
      rawMarker = projectLoader_.getTopicValue(obj.properties.setpointTopic);
    } else if (isSettableLevel(obj) && !obj.properties.writeTopic.isEmpty()) {
      // A dial that can be set rests its handle on the reported value when
      // nothing is asked, as a slider does: the handle is what says it can be
      // moved. Until 2026-09-28 a ring left that fallback out, for fear of a
      // handle standing outside a ring that reserved no room - which the
      // can-have-handle test above already rules out: only a ring with a write or
      // setpoint topic gets here, and such a ring reserves the room. A gauge
      // never rests one, whatever topics it still carries.
      rawMarker = valueStr;
    }
  }
  String trimmedMarker = rawMarker;
  trimmedMarker.trim();
  const bool hasSetpoint = !trimmedMarker.isEmpty();
  float setpointPercent = 0.0f;
  if (hasSetpoint) {
    setpointPercent = interpolateCalibration(rawMarker.toFloat(), obj.properties.calibrationPoints);
    if (setpointPercent < 0.0f) setpointPercent = 0.0f;
    if (setpointPercent > 100.0f) setpointPercent = 100.0f;
  }

  const int32_t size = min(obj.width, obj.height);
  if (size <= 0) return true;
  // `thickness` is the arc's own name and became the bar's too on 2026-09-22 -
  // one thing, one name, and one default of 16 on both. It was the ring's own
  // 22 while a ring was thought to need more presence, and that survived
  // exactly as long as it took to put a dial and a slider on one screen.
  // `barThickness` is still read (ProjectLoader), because the projects in the
  // van are full of it and nothing here rewrites a file someone else owns.
  const int32_t thickness = arcThicknessFor(obj, size);

  // How far the ring sits inside the object's own edge: room for the handle,
  // which lies across the band and stands out of it on both sides. With the
  // band touching the edge the outer half of the handle would fall outside the
  // object, and an object that draws past its own rectangle is clipped by the
  // designer's buffer, by the app's box and by this renderer's object rect
  // alike.
  //
  // Reserved whenever the object CAN have a handle rather than while one is
  // being drawn, so the ring does not jump inwards the moment a value arrives.
  // On an object too small to give up the room, the reservation gives way
  // before the band does.
  const int32_t inset = arcInsetFor(obj, size, thickness);

  // Two clock positions and a direction become one clockwise sector, so the
  // rasterizer only ever deals with clockwise sweeps. A counter-clockwise
  // dial is the same sector filled from its other end.
  const int32_t minA = ((obj.properties.minAngle % 360) + 360) % 360;
  const int32_t maxA = ((obj.properties.maxAngle % 360) + 360) % 360;
  const bool counterClockwise = obj.properties.arcDirection == "ccw";
  const int32_t startDeg = counterClockwise ? maxA : minA;
  int32_t spanDeg = counterClockwise ? ((minA - maxA + 360) % 360) : ((maxA - minA + 360) % 360);
  // Equal positions mean a full ring. An arc of zero length is not something
  // anyone builds on purpose, so the ambiguous case gets the useful reading.
  if (spanDeg == 0) spanDeg = 360;

  const int32_t start64 = startDeg * ArcRaster::ANGLE_SCALE;
  const int32_t sweep64 = spanDeg * ArcRaster::ANGLE_SCALE;

  const int32_t filled64 = ArcRaster::sweepForPercent(sweep64, fillPercent);
  const int32_t fillStart64 = counterClockwise ? start64 + sweep64 - filled64 : start64;

  ArcRaster::RingGeometry geom;
  geom.size = size;
  geom.thickness = thickness;
  geom.inset = inset;
  geom.track = ArcRaster::makeSector(start64, sweep64);
  geom.fill = ArcRaster::makeSector(fillStart64, filled64);
  // Rounded pill ends: the band is every point within half a thickness of its
  // centreline, which past the last angle is a disc. A ring that goes all the
  // way round has no ends to round.
  const ArcRaster::Caps ends = ArcRaster::caps(size, thickness, inset, start64, sweep64);
  geom.hasStartCap = ends.has;
  geom.hasEndCap = ends.has;
  geom.startCap = ends.startCap;
  geom.endCap = ends.endCap;
  // A cap belongs to the fill when the fill actually reaches that end.
  geom.startCapFilled = filled64 > 0 && fillStart64 == start64;
  geom.endCapFilled = filled64 > 0 && fillStart64 + filled64 >= start64 + sweep64;
  if (hasSetpoint) {
    // The setpoint is the slider's handle, not a wedge of the ring: a pill
    // lying across the band, standing out of it on both sides, with a gap cut
    // either side of it - and the gap takes its pixels from the ring, not from
    // the handle, which is what separates the two.
    const int32_t at = ArcRaster::sweepForPercent(sweep64, setpointPercent);
    const int32_t angle64 = counterClockwise ? start64 + sweep64 - at : start64 + at;
    if (isSettableLevel(obj)) {
      geom.handle = ArcRaster::handleBand(size, thickness, inset, angle64, sweep64);
      geom.hasHandle = true;
    } else {
      // A gauge cannot be moved, so it points instead: a triangle outside the
      // ring, in the room the inset keeps (arcPointerBand in the designer).
      geom.hasPointer = ArcRaster::pointerBand(size, thickness, inset, angle64, &geom.pointer);
    }
  }

  // One colour the author sets; everything else follows from it and from what
  // the ring stands on - the same rule, and the same function, the bar uses for
  // its own track (docs/2026-09-22-arc-look.md). The ring has no background, no
  // track colour and no marker colour of its own any more: those three stay in
  // old project files and are simply not read.
  const String fillHex = materialcolors::applyDepth(
      obj.properties.fillColor.isEmpty() ? String("#4CAF50") : obj.properties.fillColor);
  const levelshape::LevelTrackPaint look =
      levelshape::trackPaint(obj.properties.trackColor, obj.properties.trackEdgeColor, fillHex, currentScreenBgHex_);
  geom.framed = look.framed;
  geom.frameWidth = look.edgeWidth;
  geom.framedBody = look.body;

  // What the anti-aliased edges mix into: the screen's own colour, which the
  // designer knows too, so both mix into the same thing. Over a background
  // image that is an approximation - but it is the same approximation on both
  // sides, so the comparison stays exact and only the eye can tell.
  const ArcRaster::Rgb565 mix = ArcRaster::toRgb565(mixInto);
  const ArcRaster::Rgb565 fillColour = ArcRaster::toRgb565(parseHexColor(fillHex));
  // Where the mixed track cannot be told from the background, the band is drawn
  // as an outline in the bar's own colour instead of a body in a colour nobody
  // would see. A track you cannot see is not a track.
  const ArcRaster::Rgb565 trackColour =
      ArcRaster::toRgb565(parseHexColor(look.framed ? look.edge : look.track));
  // Inside a theme's edge the track keeps its colour (trackPaint's body).
  const ArcRaster::Rgb565 bodyColour = ArcRaster::toRgb565(parseHexColor(look.track));
  // A handle a finger can move keeps the bar's own colour - handle and filled
  // track are one object that the gap separates - and one that only reports the
  // installation's target takes the track's colour and steps back. The user,
  // on the picture: "seine dimmed farbe sagt mir dass ich ihn nicht bewegen
  // kann". Where the track is an outline there is no quiet colour to step back
  // into, so every handle stays solid (handleColourFor in the designer).
  const bool settable = !obj.properties.writeTopic.isEmpty();
  const ArcRaster::Rgb565 handleColour =
      ArcRaster::toRgb565(parseHexColor((settable || look.framed) ? fillHex : look.track));
  // A gauge's pointer is in the text's colour, as the bar's is.
  const ArcRaster::Rgb565 pointerColour = ArcRaster::toRgb565(parseHexColor(materialcolors::applyDepth(
      obj.properties.textColor.isEmpty() ? String("#000000") : obj.properties.textColor)));

  // The theme's look for a fill that is its accent (designer lib/themes.ts):
  // the fill runs from fillColor to fillEndColor along the scale, and a weak
  // glow lies around it. Steps are counted from the scale's zero, whichever
  // way it fills. One renderer, one task: the 1 KB of boundaries lives here
  // rather than on the stack.
  static ArcRaster::Gradient gradient;
  const bool hasGradient = !obj.properties.fillEndColor.isEmpty() && !look.framed;
  if (hasGradient) ArcRaster::makeGradient(start64, sweep64, &gradient);
  const ArcRaster::Rgb565 endColour =
      hasGradient ? ArcRaster::toRgb565(parseHexColor(materialcolors::applyDepth(obj.properties.fillEndColor)))
                  : fillColour;
  const int32_t lastStep = ArcRaster::stepOfOffset(filled64 > 1 ? filled64 - 1 : 0, sweep64);
  // Every step's colour once, rather than three divisions per pixel.
  static ArcRaster::Rgb565 stepColour[ArcRaster::GRADIENT_STEPS];
  if (hasGradient) {
    for (int32_t i = 0; i < ArcRaster::GRADIENT_STEPS; i++) {
      stepColour[i] = levelglow::gradient565(fillColour, endColour, i, ArcRaster::GRADIENT_STEPS);
    }
  }
  const int32_t glowLevels = levelglow::glowPx(obj.properties.glow);
  const bool hasGlow = glowLevels > 0 && filled64 > 0 && !noValue && !look.framed;
  ArcRaster::Glow glow;
  if (hasGlow) {
    glow.rMid = ArcRaster::midRadius(size, thickness, inset);
    glow.half = thickness * ArcRaster::SUBPIXEL_SCALE / 2;
    glow.fill = geom.fill;
    ArcRaster::pointAt(size, thickness, inset, fillStart64, &glow.endX[0], &glow.endY[0]);
    ArcRaster::pointAt(size, thickness, inset, fillStart64 + filled64, &glow.endX[1], &glow.endY[1]);
    glow.stepFrom = 0;
    glow.stepTo = lastStep;
    geom.glowReach = glowLevels * ArcRaster::SUBPIXEL_SCALE;
  }
  // A handle a finger moves takes the colour of the fill where it stands.
  ArcRaster::Rgb565 handleFill = handleColour;
  if (hasGradient && geom.hasHandle && isSettableLevel(obj)) {
    handleFill = levelglow::gradient565(
        fillColour, endColour,
        ArcRaster::stepOfOffset(ArcRaster::sweepForPercent(sweep64, setpointPercent), sweep64),
        ArcRaster::GRADIENT_STEPS);
  }
  const int32_t centre = size * ArcRaster::SUBPIXEL_SCALE / 2;

  // Nur den geclippten Ausschnitt durchrechnen. pixelBands() tastet jedes
  // Pixel 16-fach ab; auf einem 360er Ring sind das rund 380.000 Sektortests
  // pro Vollbild und damit gemessene 250ms. Beim Neuzeichnen eines
  // Ausschnitts faellt davon nur der Bruchteil an, der wirklich anders wird.
  // Ohne Clip unveraendert das ganze Quadrat.
  int32_t pxFrom = 0, pxTo = size, pyFrom = 0, pyTo = size;
  if (canvas_->clipActive()) {
    const int32_t cx0 = (int32_t)canvas_->clipX() - obj.x;
    const int32_t cy0 = (int32_t)canvas_->clipY() - obj.y;
    const int32_t cx1 = cx0 + (int32_t)canvas_->clipW();
    const int32_t cy1 = cy0 + (int32_t)canvas_->clipH();
    if (cx0 > pxFrom) pxFrom = cx0;
    if (cy0 > pyFrom) pyFrom = cy0;
    if (cx1 < pxTo) pxTo = cx1;
    if (cy1 < pyTo) pyTo = cy1;
  }

  // Cycle counters, live only while a diagnosis run has enabled profiling.
  // esp_cpu_get_cycle_count() is one register read; wrapping 130,000
  // iterations with it costs far less than the thing being measured.
  const bool profiling = RenderProfile::enabled();
  if (profiling) { ArcRaster::probeEnabled() = true; ArcRaster::probeReset(); }
  uint32_t bandsCycles = 0, drawCycles = 0, shadeCycles = 0, pixelsSeen = 0, pixelsDrawn = 0;

  // Nothing but the ring is painted: the object has no background of its own
  // any more, so whatever is behind it - a screen colour, a background image -
  // shows through everywhere the band is not (docs/2026-09-22-arc-look.md).
  for (int32_t py = pyFrom; py < pyTo; py++) {
    // Which pixels of this row the ring can reach at all. Everything outside
    // is empty by the same test pixelBands() makes before anything else, so
    // walking it one pixel at a time only paid to be told nothing was there:
    // 105,236 of this ring's 129,600 pixels, and a third of the arc's time.
    const ArcRaster::RowSpan span = ArcRaster::rowSpan(geom, py);
    const int32_t from = span.x0 > pxFrom ? span.x0 : pxFrom;
    const int32_t to = span.x1 + 1 < pxTo ? span.x1 + 1 : pxTo;
    const int32_t holeFrom = span.holeX0 > from ? span.holeX0 : from;
    const int32_t holeTo = span.holeX1 < to - 1 ? span.holeX1 : to - 1;
    const bool hasHole = span.holeX1 >= span.holeX0 && holeTo >= holeFrom;

    for (int32_t px = from; px < to; px++) {
      // Jumping the hole rather than testing every pixel of it. The for-step
      // then lands on the first pixel past it.
      if (hasHole && px >= holeFrom && px <= holeTo) {
        px = holeTo;
        continue;
      }
      uint32_t c0 = profiling ? esp_cpu_get_cycle_count() : 0;
      const ArcRaster::PixelBands bands = ArcRaster::pixelBands(geom, px, py);
      if (profiling) {
        bandsCycles += esp_cpu_get_cycle_count() - c0;
        pixelsSeen++;
      }
      const int32_t covered = bands.fill + bands.track + bands.body + bands.handle + bands.pointer;
      // The pixel's centre, in 1/8 pixel from the ring's centre.
      const int32_t cx = px * ArcRaster::SUBPIXEL_SCALE + ArcRaster::SUBPIXEL_SCALE / 2 - centre;
      const int32_t cy = py * ArcRaster::SUBPIXEL_SCALE + ArcRaster::SUBPIXEL_SCALE / 2 - centre;
      uint32_t s0 = profiling ? esp_cpu_get_cycle_count() : 0;
      const int32_t glowK =
          hasGlow && covered < ArcRaster::COVERAGE_MAX ? ArcRaster::glowLevel(glow, cx, cy, glowLevels) : 0;

      // Nothing of the ring here: the pixel is left alone, which is what the
      // ring floating on the screen means - skip the draw, never mix.
      if (covered == 0 && glowK == 0) {
        if (profiling) shadeCycles += esp_cpu_get_cycle_count() - s0;
        continue;
      }
      // The pixel's step, once, and only where something takes a colour from
      // it: the glow, or a fill.
      int32_t step = 0;
      if (hasGradient && (glowK > 0 || bands.fill > 0)) {
        step = ArcRaster::gradientStep(gradient, cx, cy);
        if (counterClockwise) step = ArcRaster::GRADIENT_STEPS - 1 - step;
      }
      ArcRaster::Rgb565 base = mix;
      if (glowK > 0) {
        // The glow is mixed into what already stands here - the screen, an
        // icon behind the ring, another control: drawn back to front, it is
        // on the canvas already.
        ArcRaster::Rgb565 glowColour = fillColour;
        if (hasGradient) {
          int32_t glowStep = step;
          if (glowStep < glow.stepFrom) glowStep = glow.stepFrom;
          if (glowStep > glow.stepTo) glowStep = glow.stepTo;
          glowColour = stepColour[glowStep];
        }
        const ArcRaster::Rgb565 under = ArcRaster::toRgb565(canvas_->getPixel(obj.x + px, obj.y + py));
        base = levelglow::blend565(under, glowColour, levelglow::ALPHA[glowK]);
        if (covered == 0) {
          canvas_->drawPixel(obj.x + px, obj.y + py, ArcRaster::packRgb565(base));
          if (profiling) shadeCycles += esp_cpu_get_cycle_count() - s0;
          continue;
        }
      }

      ArcRaster::Rgb565 fillHere = fillColour;
      if (hasGradient && bands.fill > 0) fillHere = stepColour[step > lastStep ? lastStep : step];
      if (profiling) shadeCycles += esp_cpu_get_cycle_count() - s0;
      const uint16_t mixed = ArcRaster::blendBands(fillHere, bands.fill, trackColour, bands.track, bodyColour,
                                                   bands.body, handleFill, bands.handle, pointerColour,
                                                   bands.pointer, base, (uint8_t)(ArcRaster::COVERAGE_MAX - covered));
      uint32_t d1 = profiling ? esp_cpu_get_cycle_count() : 0;
      canvas_->drawPixel(obj.x + px, obj.y + py, mixed);
      if (profiling) {
        drawCycles += esp_cpu_get_cycle_count() - d1;
        pixelsDrawn++;
      }
    }
  }

  if (profiling) {
    const uint32_t mhz = (uint32_t)(esp_cpu_get_cycle_count() ? 240 : 240);
    RenderProfile::add("  arc: pixelBands", bandsCycles / mhz);
    RenderProfile::add("  arc: drawPixel", drawCycles / mhz);
    // Gradient and glow: the step, the glow's reach and the pixel read back.
    RenderProfile::add("  arc: gradient and glow", shadeCycles / mhz);
    RenderProfile::note("  arc: " + String(pixelsSeen) + " px seen, " + String(pixelsDrawn) + " drawn");
    ArcRaster::probeEnabled() = false;
    const ArcRaster::Probe& pr = ArcRaster::probe();
    RenderProfile::add("  arc: early x" + String(pr.earlyCalls), pr.earlyCycles / 240);
    RenderProfile::add("  arc: sampled x" + String(pr.sampledCalls), pr.sampledCycles / 240);
    RenderProfile::add("  arc: uniform x" + String(pr.uniformCalls), pr.uniformCycles / 240);
  }

  // The numbers: the big one is the COMMANDED value
  // - where the handle points, and what a finger just changed - and the
  // measured one appears under it in brackets, smaller, only when it says
  // something the big one does not. A ring puts the two one above the other
  // rather than side by side: it has the room.
  if (noValue) return true;
  const bool asPercent = obj.properties.displayValue == "percentage";
  if (!asPercent && obj.properties.displayValue != "value") return true;

  // Round, don't truncate - the same Math.round the bar's own value text
  // matches, for the same reason.
  const String measured = asPercent ? String((int)(fillPercent + 0.5f)) + "%" : valueStr;
  const String commanded =
      hasSetpoint ? (asPercent ? String((int)(setpointPercent + 0.5f)) + "%" : rawMarker) : measured;
  if (commanded.isEmpty()) return true;
  const bool showsSub = hasSetpoint && measured != commanded;

  // Centred exactly as renderLevelIndicator centres its own value, so the
  // two siblings cannot drift apart on the one thing they both do.
  const BdfFont* font = fontFor(obj.properties.fontId);
  bool textTransparent = false;
  const uint16_t textColor = parseHexColor(
      obj.properties.textColor.isEmpty() ? "#000000" : obj.properties.textColor, &textTransparent);
  if (textTransparent || font == nullptr) return true;

  const int16_t fontAscent = getFontAscentById(obj.properties.fontId);
  const int16_t line = fontAscent + getFontDescentById(obj.properties.fontId);

  // The bracketed number is written in another file, not the same one drawn
  // smaller: a BDF face cannot be scaled at all, so "smaller" has to mean
  // "another font", which is the rule the bar's own sub-number follows
  // (levelSubFontId, two thirds of the object's own line).
  String subText;
  const BdfFont* subFont = nullptr;
  int16_t subAscent = 0;
  int16_t subLine = 0;
  String subId;
  if (showsSub) {
    subText = "(" + measured + ")";
    subId = levelSubFontId(obj);
    const BdfFont* candidate = fontFor(subId);
    if (candidate != nullptr) {
      subFont = candidate;
      subAscent = getFontAscentById(subId);
      subLine = subAscent + getFontDescentById(subId);
    }
  }

  // The pair is centred together, so the big number does not jump the moment a
  // setpoint arrives.
  const int16_t block = line + (subFont != nullptr ? subLine + kArcSubGap : 0);
  const float top = obj.y + (obj.height - block) / 2.0f;

  canvas_->setClip(obj.x, obj.y, obj.width, obj.height);
  const int16_t textWidth = measureTrueTextWidth(commanded, font);
  font->drawText(*canvas_, commanded, (int16_t)(obj.x + jsRound((obj.width - textWidth) / 2.0f)),
                 (int16_t)jsRound(top + fontAscent), textColor);
  if (subFont != nullptr) {
    const int16_t subWidth = measureTrueTextWidth(subText, subFont);
    subFont->drawText(*canvas_, subText, (int16_t)(obj.x + jsRound((obj.width - subWidth) / 2.0f)),
                      (int16_t)jsRound(top + line + kArcSubGap + subAscent), textColor);
  }
  canvas_->clearClip();

  return true;
}

bool ColorScreenRenderer::renderMQTTIconField(const ScreenObject& obj) {
  String valueStr = projectLoader_.getTopicValue(obj.properties.topic);
  if (valueStr.isEmpty()) return false;

  String iconPath = getIconPathForValue(obj, valueStr);
  if (iconPath.isEmpty()) return false;

  if (!ColorAssetLoader::drawBMPToCanvas(canvas_, iconPath, obj.x, obj.y)) {
    // Placeholder so a missing/bad asset is visibly wrong, not invisible.
    canvas_->fillRect(obj.x + 2, obj.y + 2, obj.width - 4, obj.height - 4, 0x0000);
    canvas_->drawRect(obj.x, obj.y, obj.width, obj.height, 0x0000);
  }
  return true;
}

bool ColorScreenRenderer::renderIcon(const ScreenObject& obj) {
  // A live icon (contract 2.6): the icon of the branch that applies, its
  // dark bitmap under the dark theme. A branch without an icon - No value
  // yet left empty - draws nothing, and that is not a failure.
  if (!obj.properties.liveIconId.isEmpty()) {
    for (const livevalue::LiveValue& lv : obj.properties.liveValues) {
      if (lv.id != obj.properties.liveIconId.c_str()) continue;
      const livevalue::Applies applies = livevalue::evaluate(lv, liveSourceValue(lv.source));
      const livevalue::Result* result = applies.result;
      if (result == nullptr || result->kind != livevalue::ResultKind::Icon) return true;
      const bool dark = projectLoader_.getProject().themeDark && !result->pathDark.empty();
      const String path((dark ? result->pathDark : result->path).c_str());
      if (path.isEmpty()) return true;
      if (!ColorAssetLoader::drawBMPToCanvas(canvas_, path, obj.x, obj.y)) {
        canvas_->fillRect(obj.x + 2, obj.y + 2, obj.width - 4, obj.height - 4, 0x0000);
        canvas_->drawRect(obj.x, obj.y, obj.width, obj.height, 0x0000);
        return false;
      }
      return true;
    }
  }
  if (obj.path.isEmpty()) return false;
  if (!ColorAssetLoader::drawBMPToCanvas(canvas_, obj.path, obj.x, obj.y)) {
    canvas_->fillRect(obj.x + 2, obj.y + 2, obj.width - 4, obj.height - 4, 0x0000);
    canvas_->drawRect(obj.x, obj.y, obj.width, obj.height, 0x0000);
    return false;
  }
  return true;
}

// Draws pathActive while pressed (isPressed, driven by main.cpp's touch
// hit-testing - see ColorScreenRenderer.h's renderScreen() comment), else
// pathNormal. Both bitmaps are already fully rendered by the designer's
// export (drop shadow / 3D-press offset, border, label text baked in) -
// this only needs to decode and blit whichever one applies, not
// reconstruct any of that.
bool ColorScreenRenderer::renderSoftwareButton(const ScreenObject& obj, bool isPressed) {
  // Faellt auf das normale Bild zurueck, wenn es kein gedruecktes gibt.
  // Sonst zeichnete ein Druck GAR NICHTS und der Knopf verschwaende, solange
  // der Finger liegt - und der Designer zeigt in diesem Fall ebenfalls das
  // normale Bild, die Pixelgleichheit wird dadurch also hergestellt und
  // nicht gebrochen. Vor dem 2026-09-10 war das unerreichbar: kein Board
  // hat je einen gedrueckten Zustand angefordert.
  const String& path = (isPressed && !obj.pathActive.isEmpty()) ? obj.pathActive : obj.pathNormal;
  if (path.isEmpty()) return false;
  if (!ColorAssetLoader::drawBMPToCanvas(canvas_, path, obj.x, obj.y)) {
    canvas_->fillRect(obj.x + 2, obj.y + 2, obj.width - 4, obj.height - 4, 0x0000);
    canvas_->drawRect(obj.x, obj.y, obj.width, obj.height, 0x0000);
    return false;
  }
  return true;
}

int ColorScreenRenderer::getActiveSwitchStateIndex(const ScreenObject& obj) const {
  if (obj.properties.topic.isEmpty()) return -1;
  String topicValue = projectLoader_.getTopicValue(obj.properties.topic);
  topicValue.trim();
  // No value, no active segment - not even one whose readValue is empty.
  if (topicValue.isEmpty()) return -1;
  for (size_t i = 0; i < obj.properties.states.size(); i++) {
    String readValue = obj.properties.states[i].readValue;
    readValue.trim();
    if (readValue == topicValue) return (int)i;
  }
  return -1;
}

// Segmented control, one segment per obj.properties.states entry - mirrors
// the designer's renderSwitch() (render-switch.ts) as closely as this
// firmware's text/color primitives allow: same draw order, same active-
// state resolution (getActiveSwitchStateIndex), same per-segment clipping
// so an oversized font can't bleed into a neighboring segment (2026-08-13
// designer-side finding, ported here too). No tap/MQTT-write logic here -
// that's main.cpp's job (findSwitchSegmentAt + the pending/timeout state
// machine); this function only ever draws whatever pendingStateIndex/
// pressedStateIndex it's told, same "renderer just draws, main.cpp owns the
// state machine" split as renderSoftwareButton's isPressed.
//
// 2026-08-25: the active state is a marker bar along the top edge, not a
// filled segment. The fill carried three meanings at once through
// activeBackgroundColor - solid fill = active, 3px ring = pending, 1px ring
// = pressed - and none of them survived being put next to a screen of
// bar-marked tiles. There is now one shape with three appearances, and the
// rings are gone entirely:
//
//   absent  - a confirmed state carrying no marker
//   hollow  - unconfirmed: finger down, or written and waiting for the
//             retained value (the designer never draws this - nobody is
//             touching the canvas, which is also why a HIL comparison
//             against it stays valid)
//   solid   - a confirmed state carrying a marker
//
// Hollow is drawn regardless of showMarker. Requesting a state that carries
// no marker would otherwise produce no feedback at all for the up-to-3s the
// write is in flight.

// Marker geometry. Every value is a whole number with no division behind
// it, and every one of them also appears in render-switch.ts under the same
// name. Stated values cannot drift; computed ones get to disagree about
// rounding, which is how a shared shape turns into a HIL pixel diff on
// every Switch on the screen at once.
static const int kSwitchBarHeight = 10;
static const int kSwitchBarTopInset = 4;
static const int kSwitchBarSideInset = 6;
static const int kSwitchBarRadius = 3;
// Reserved whether or not a bar is currently drawn, so a Switch does not
// reflow its icon and label the moment it is switched on.
static const int kSwitchBarBand = kSwitchBarTopInset + kSwitchBarHeight;

// One marker bar, centered horizontally in [x, x + width).
//
// The width clamp guards objects saved before the designer's resize
// minimums existed: a five-state Switch 60px wide gives each segment 12px,
// and 12 - 2*6 is zero - a control that silently says nothing about its own
// state. One integer max() is cheaper than any amount of warning UI, and
// render-switch.ts's drawBar() does exactly the same thing.
bool ColorScreenRenderer::renderSwitch(const ScreenObject& obj, int pendingStateIndex, int pressedStateIndex) {
  // The Switch in its two Material 3 forms, ported from the designer's
  // lib/switch-shape.ts through src/project/SwitchShape.h
  // (docs/2026-09-20-switch-look.md): a connected button group, or a track with
  // a knob. No box, no border, no dividers and no marker bar - one colour, and
  // how loud the chosen state is.
  const std::vector<SwitchState>& states = obj.properties.states;
  const int count = (int)states.size();
  const BdfFont* font = fontFor(obj.properties.fontId);

  if (count == 0) {
    const BdfFont* placeholderFont = fontStore_.get(fontStore_.defaultFontId());
    const String placeholder = "No states defined";
    if (placeholderFont != nullptr) {
      const int16_t textWidth = measureTrueTextWidth(placeholder, placeholderFont);
      const int16_t textX = obj.x + jsRound((obj.width - textWidth) / 2.0f);
      canvas_->setClip(obj.x, obj.y, obj.width, obj.height);
      placeholderFont->drawText(*canvas_, placeholder, textX, obj.y + obj.height / 2 + 4, 0x8410);
      canvas_->clearClip();
    }
    return true;
  }

  LevelFontMetrics metrics = levelMetrics(obj);
  const SwitchLook look = switchshape::look(obj, currentScreenBgHex_);
  const int activeIndex = getActiveSwitchStateIndex(obj);

  // What a finger here asked this switch to become, keyed by the topic an
  // answer would arrive on - the same bookkeeping a settable level's marker
  // uses. The firmware's own pending/pressed indices say the same thing while
  // a write is in flight.
  int askedIndex = -1;
  if (!askedTopic_.isEmpty() && askedTopic_ == obj.properties.topic && !askedValue_.isEmpty()) {
    for (int i = 0; i < count; i++) {
      String read = states[i].readValue;
      read.trim();
      String asked = askedValue_;
      asked.trim();
      if (read == asked) askedIndex = i;
    }
  }
  if (askedIndex < 0) askedIndex = pendingStateIndex;

  auto colourOf = [&](const String& hex) { return parseHexColor(hex); };
#if !PILL_ANTIALIASED
  // The whole-pixel path, unchanged since before the rasterizer existed and
  // kept line for line: below 24 bit there is nothing to mix into, so this is
  // still the picture (PillRaster.h says why that is a decision).
  //
  // One rounded rectangle, whose two ends can carry different radii - which is
  // what a button in a connected group is since 2026-09-21 (SwitchShape.h).
  // GFX has no primitive for that, so the shape is drawn twice and each pass is
  // clipped to the half its own end belongs to. Safe because fillRoundRect
  // clamps a radius to half the short side, which is never more than half the
  // width: neither end's rounding can reach across the split. The rasterised
  // path below says the same thing in one band, with rLow and rHigh.
  auto fillPill = [&](const SwitchRect& r, uint16_t colour) {
    if (!r.valid()) return;
    if (r.rightRadius() == r.r) {
      canvas_->fillRoundRect(r.x, r.y, r.w, r.h, r.r, colour);
      return;
    }
    const int split = r.w / 2;
    canvas_->setClip(r.x, r.y, split, r.h);
    canvas_->fillRoundRect(r.x, r.y, r.w, r.h, r.r, colour);
    canvas_->setClip(r.x + split, r.y, r.w - split, r.h);
    canvas_->fillRoundRect(r.x, r.y, r.w, r.h, r.rightRadius(), colour);
    canvas_->clearClip();
  };
  auto fillRect = [&](const SwitchRect& r, const String& hex) { fillPill(r, colourOf(hex)); };
  // A ring cut out of a filled shape rather than stroked: a thin stroke breaks
  // up wherever the picture is later cut to one bit, and cutting leaves what is
  // behind the control visible.
  //
  // What the ring encloses is the CALLER's to say. It used to be the
  // container's surface whatever was being ringed, which erased the fill of a
  // chosen segment the moment a finger went down on it and painted a knob
  // track's inside in a colour from the other form - the designer puts the
  // chosen colour and the track's own colour there (render-switch.ts), and the
  // anti-aliased path below already did.
  auto ringRect = [&](const SwitchRect& r, int thickness, const String& hex, const String& insideHex) {
    if (!r.valid()) return;
    fillPill(r, colourOf(hex));
    const int t = thickness > 0 ? thickness : 1;
    if (r.w > 2 * t && r.h > 2 * t) {
      SwitchRect inner;
      inner.x = r.x + t;
      inner.y = r.y + t;
      inner.w = r.w - 2 * t;
      inner.h = r.h - 2 * t;
      inner.r = r.r - t > 0 ? r.r - t : 0;
      inner.rRight = r.rightRadius() - t > 0 ? r.rightRadius() - t : 0;
      fillPill(inner, colourOf(insideHex));
    }
  };
#else
  // Everything this control is made of is a pill: the container, a segment,
  // the track, the knob. They are described rather than painted, because the
  // rasterizer needs them all at once to give each sub-sample to exactly one
  // of them (PillRaster.h).
  //
  // A SwitchRect carries a radius for each end since the connected group's
  // geometry came across (SwitchShape.h): a button is round on the outside and
  // barely rounded where it faces its neighbour, and rightRadius() answers the
  // one radius for everything else.
  auto wholePill = [](const SwitchRect& r, uint16_t colour) {
    PaintedPill out;
    out.band.x = r.x;
    out.band.y = r.y;
    out.band.w = r.w;
    out.band.h = r.h;
    out.band.rLow = r.r;
    out.band.rHigh = r.rightRadius();
    out.colour = colour;
    out.paints = true;
    return out;
  };
  // What a ring leaves untouched - the same arithmetic the whole-pixel
  // ringRect uses for its inside, so a ring comes out where it always did.
  // `paints` false is a hole: the run claims its pixels and paints nothing,
  // which is how what lies behind the control stays visible through it.
  auto insidePill = [](const SwitchRect& r, int thickness, uint16_t colour, bool paints) {
    const int t = thickness > 0 ? thickness : 1;
    PaintedPill out;
    out.band.x = r.x + t;
    out.band.y = r.y + t;
    out.band.w = r.w - 2 * t;
    out.band.h = r.h - 2 * t;
    out.band.rLow = r.r - t > 0 ? r.r - t : 0;
    out.band.rHigh = r.rightRadius() - t > 0 ? r.rightRadius() - t : 0;
    out.colour = colour;
    out.paints = paints;
    return out;
  };
  const uint16_t pillMix = parseHexColor(materialcolors::applyDepth(currentScreenBgHex_));
#endif
  auto drawStateText = [&](const String& text, const SwitchRect& clip, int x, int baseline, const String& hex) {
    if (text.isEmpty() || font == nullptr || !clip.valid()) return;
    canvas_->setClip(clip.x, clip.y, clip.w, clip.h);
    font->drawText(*canvas_, text, (int16_t)x, (int16_t)baseline, colourOf(hex));
    canvas_->clearClip();
  };
  auto widthOf = [&](const String& text) -> int {
    if (font == nullptr || text.isEmpty()) return 0;
    return measureTrueTextWidth(text, font);
  };

  if (!switchshape::isKnobForm(obj)) {
    const SwitchRect box = switchshape::container(obj);
    std::vector<SwitchRect> segs;
    switchshape::segments(obj, count, segs);

#if PILL_ANTIALIASED
    // Every shape at once, in priority order, through one rasterizer: a
    // sub-sample belongs to the first run that contains it and to no other.
    // Drawn one over the other, each rounded end would carry a rim of whatever
    // it covers - which is what the arc was fixed for, and what a
    // stair-stepped pill beside an anti-aliased ring asked about (2026-09-22).
    std::vector<PaintedPill> painted;
    for (int i = 0; i < count && i < (int)segs.size(); i++) {
      const SwitchRect& seg = segs[i];
      const bool chosen = i == activeIndex;
      // A finger on a segment, and a tap whose answer has not come back: the
      // same ring. Both mean "this is not what is reported - yet".
      if (i == askedIndex || i == pressedStateIndex) {
        // A ring is its outer pill with the inside taken back, so what the
        // ring encloses has to be said out loud: the chosen pill, the
        // container, or - where the container is only an outline - nothing at
        // all (switchshape::ringFill, which the designer and the Android app
        // read from their own copy of the same rule). Nothing at all is a run
        // that claims its sub-samples and paints none of them.
        const String inside = switchshape::ringFill(look, chosen);
        if (inside.isEmpty()) painted.push_back(insidePill(seg, 2, 0, false));
        else painted.push_back(insidePill(seg, 2, colourOf(inside), true));
        painted.push_back(wholePill(seg, colourOf(look.ring)));
        continue;
      }
      if (chosen) painted.push_back(wholePill(seg, colourOf(look.chosen)));
    }
    if (look.surfaceFramed) {
      painted.push_back(insidePill(box, 1, 0, false));
      painted.push_back(wholePill(box, colourOf(materialcolors::applyDepth(switchshape::colorOf(obj)))));
    } else {
      painted.push_back(wholePill(box, colourOf(look.surface)));
    }
    paintPills(painted, pillMix);
#else
    // The container's outline encloses whatever the screen is: there is no
    // colour behind a framed container, and this path cannot leave a hole.
    if (look.surfaceFramed) {
      ringRect(box, 1, materialcolors::applyDepth(switchshape::colorOf(obj)), currentScreenBgHex_);
    } else {
      fillRect(box, look.surface);
    }
    for (int i = 0; i < count && i < (int)segs.size(); i++) {
      const SwitchRect& seg = segs[i];
      const bool chosen = i == activeIndex;
      if (chosen) fillRect(seg, look.chosen);
      // A finger on a segment, and a tap whose answer has not come back: the
      // same ring. Both mean "this is not what is reported - yet".
      //
      // A ring is its outer pill with the inside taken back, so what the ring
      // encloses has to be said out loud: the chosen segment keeps its own
      // colour inside the ring - it is still the reported state - and an
      // unchosen one shows the container, or the screen where the container is
      // only an outline. One rule, in switchshape::ringFill; "nothing behind
      // it" is the screen's own colour on this path, which cannot leave a hole.
      if (i == askedIndex || i == pressedStateIndex) {
        const String fill = switchshape::ringFill(look, chosen);
        ringRect(seg, 2, look.ring, fill.isEmpty() ? currentScreenBgHex_ : fill);
      }
    }
#endif

    // The icons and the labels, once the shapes are down: the same loop
    // whichever way they were painted.
    for (int i = 0; i < count && i < (int)segs.size(); i++) {
      const SwitchRect& seg = segs[i];
      const bool chosen = i == activeIndex;
      const String ink = chosen ? look.onChosen : look.onSurface;
      const String& iconPath =
          (chosen && !states[i].iconPathActive.isEmpty()) ? states[i].iconPathActive : states[i].iconPath;
      const String label = states[i].label;
      const SwitchContent content = switchshape::content(seg, metrics, !iconPath.isEmpty(), widthOf(label));
      canvas_->setClip(seg.x, seg.y, seg.w, seg.h);
      if (content.hasIcon && content.icon.valid()) {
        ColorAssetLoader::drawBMPToCanvas(canvas_, iconPath, content.icon.x, content.icon.y);
      }
      canvas_->clearClip();
      drawStateText(label, seg, content.textX, content.baseline, ink);
    }
    return true;
  }

  // The switch form. The colour says what is reported; the knob says what was
  // asked for.
  const bool on = activeIndex >= 0 && switchshape::stateIsOn(states[activeIndex]);
  const SwitchKnobLook knobLook = switchshape::knobLook(obj, currentScreenBgHex_, on);
  const SwitchRect track = switchshape::track(obj, count);

  // The knob is worked out before anything is painted, because the rasterizer
  // wants it FIRST: it wins every sub-sample it covers, so no rim of track
  // colour is left around it. Where it is drawn has not moved - it is still on
  // top of the track - only where it is decided.
  const int shownIndex = askedIndex >= 0 ? askedIndex : activeIndex;
  const bool hasKnob = shownIndex >= 0;
  switchshape::SwitchKnob knob;
  if (hasKnob) knob = switchshape::knob(obj, count, shownIndex, pressedStateIndex >= 0, on);
  const int d = knob.r * 2;

#if PILL_ANTIALIASED
  std::vector<PaintedPill> painted;
  if (hasKnob) {
    SwitchRect circle;
    circle.x = knob.cx - knob.r;
    circle.y = knob.cy - knob.r;
    circle.w = d;
    circle.h = d;
    circle.r = knob.r;
    painted.push_back(wholePill(circle, colourOf(knobLook.knob)));
  }
  if (!knobLook.trackOutline.isEmpty()) {
    painted.push_back(insidePill(track, switchshape::kTrackOutline, colourOf(knobLook.track), true));
    painted.push_back(wholePill(track, colourOf(knobLook.trackOutline)));
  } else {
    painted.push_back(wholePill(track, colourOf(knobLook.track)));
  }
  paintPills(painted, pillMix);
#else
  // The track's own colour inside its outline, which is what the rasterised
  // path puts there and what the designer draws. It was the container's surface
  // until the ring learnt to be told - a colour from the other form entirely.
  fillRect(track, knobLook.track);
  if (!knobLook.trackOutline.isEmpty()) {
    ringRect(track, switchshape::kTrackOutline, knobLook.trackOutline, knobLook.track);
  }
#endif

  const String ink = materialcolors::onColorFor(currentScreenBgHex_);
  const SwitchRect box = switchshape::labelBox(obj, count);

  if (!hasKnob) {
    // Nothing reported: no knob, because every position belongs to a state and
    // standing somewhere would claim one nobody has reported.
    const SwitchContent content = switchshape::content(box, metrics, false, widthOf("?"));
    drawStateText("?", box, content.textX, content.baseline, ink);
    return true;
  }

#if !PILL_ANTIALIASED
  canvas_->fillRoundRect(knob.cx - knob.r, knob.cy - knob.r, d, d, knob.r, colourOf(knobLook.knob));
#endif

  const SwitchState& state = states[shownIndex];
  // Only a knob that is ON carries an icon. The quiet knob is half the
  // track's height, and an icon squeezed into it read as a smudge.
  if (on) {
    const String& iconPath = !state.iconPathActive.isEmpty() ? state.iconPathActive : state.iconPath;
    if (!iconPath.isEmpty()) {
      // switchshape::knobIcon, not three fifths worked out here: the designer
      // bakes the bitmap to that rule and the two have to be the same number.
      const SwitchRect iconBox = switchshape::knobIcon(knob);
      ColorAssetLoader::drawBMPToCanvas(canvas_, iconPath, iconBox.x, iconBox.y);
    }
  }

  const SwitchContent content = switchshape::content(box, metrics, false, widthOf(state.label));
  drawStateText(state.label, box, box.x, content.baseline, ink);
  return true;
}


// Draw order for objects that share a zIndex.
//
// The designer breaks that tie by object id (lib/object-order.ts's
// sortChildrenByZIndex: `a.zIndex - b.zIndex || a.id.localeCompare(b.id)`),
// which is a total order - two objects can never come out in a different
// sequence twice. std::sort is not stable and was given only `zIndex <`, so
// a tie here resolved however the sort felt like it, and the two sides were
// free to disagree about which of two overlapping objects covers the other.
//
// Found 2026-08-25 by the first HIL run that ever had two equal-zIndex
// objects overlapping: a label and a Switch, 1533 pixels, with the designer
// drawing the Switch on top and the device drawing the label on top. Both
// were "correct" by their own rule; only one of them had a rule.
//
// Byte comparison against localeCompare is close enough for object ids,
// which this app only ever generates out of ASCII.
static bool zOrderLess(const ScreenObject& a, const ScreenObject& b) {
  if (a.zIndex != b.zIndex) return a.zIndex < b.zIndex;
  return strcmp(a.id.c_str(), b.id.c_str()) < 0;
}

const ScreenObject* ColorScreenRenderer::getActivePanel(const ScreenObject& tabControl) const {
  String topicValue = projectLoader_.getTopicValue(tabControl.properties.topic);

  // zIndex order among the panel siblings, first match wins - same
  // "walk in order, return first match" shape as getIconPathForValue().
  std::vector<const ScreenObject*> sortedPanels;
  for (const auto& panel : tabControl.children) {
    sortedPanels.push_back(&panel);
  }
  std::sort(sortedPanels.begin(), sortedPanels.end(),
    [](const ScreenObject* a, const ScreenObject* b) { return zOrderLess(*a, *b); });

  // No value yet: the first panel, so a screen built from tabs is usable
  // before anything has arrived - and no condition gets to match an empty
  // value by accident ("< 5" would, an empty value reads as 0). Same as
  // getActivePanel() in the designer's lib/render-screen.ts.
  String trimmedValue = topicValue;
  trimmedValue.trim();
  if (!tabControl.properties.topic.isEmpty() && trimmedValue.isEmpty()) {
    return sortedPanels.empty() ? nullptr : sortedPanels[0];
  }

  for (const ScreenObject* panel : sortedPanels) {
    if (evaluateVisibilityCondition(topicValue, panel->properties.comparisonOperator, panel->properties.comparisonValue)) {
      return panel;
    }
  }
  return nullptr;
}

bool ColorScreenRenderer::renderTabControl(const ScreenObject& obj, const String& pressedButtonId,
                                            const String& pendingSwitchId, int pendingSwitchStateIndex,
                                            const String& pressedSwitchId, int pressedSwitchStateIndex) {
  switcherDrawCount_++;
  const ScreenObject* activePanel = getActivePanel(obj);
  if (activePanel == nullptr) return true;  // No panel matches - draw nothing.

  // zIndex order among the panel's own children, same as renderScreen()'s
  // top-level sort.
  std::vector<ScreenObject> sortedChildren = activePanel->children;
  std::sort(sortedChildren.begin(), sortedChildren.end(), zOrderLess);

  for (const auto& child : sortedChildren) {
    // Child coordinates are relative to the tab-control's own origin, not
    // absolute screen coordinates - offset by (obj.x, obj.y) before
    // recursing through the normal dispatch. Accumulates correctly through
    // arbitrarily deep nesting since each level's children are already
    // offset-adjusted by the time a nested tab-control applies its own.
    ScreenObject offsetChild = child;
    offsetChild.x += obj.x;
    offsetChild.y += obj.y;
    renderObject(offsetChild, pressedButtonId, pendingSwitchId, pendingSwitchStateIndex, pressedSwitchId, pressedSwitchStateIndex);
  }
  return true;
}

bool ColorScreenRenderer::renderObject(const ScreenObject& obj, const String& pressedButtonId,
                                        const String& pendingSwitchId, int pendingSwitchStateIndex,
                                        const String& pressedSwitchId, int pressedSwitchStateIndex) {
  if (obj.type == "box") return renderBox(obj);
  if (objecttypes::isLine(obj.type)) return obj.type == "live-line" ? renderMqttDataLine(obj) : renderLine(obj);
  if (obj.type == "text") return renderLabel(obj);
  if (obj.type == "live-text") return renderMQTTDataField(obj);
  // A slider is a bar with a handle and a dial a gauge with one: the same
  // renderer draws both, and the handle appears because the type says a
  // finger can move it.
  if (objecttypes::isLevel(obj.type)) return renderLevelIndicator(obj);
  if (objecttypes::isArc(obj.type)) return renderArcLevel(obj, currentScreenBg_);
  if (obj.type == "live-icon") return renderMQTTIconField(obj);
  if (obj.type == "icon") return renderIcon(obj);
  if (obj.type == "button") return renderSoftwareButton(obj, !pressedButtonId.isEmpty() && obj.id == pressedButtonId);
  if (objecttypes::isSwitch(obj.type)) {
    int pendingIndex = obj.id == pendingSwitchId ? pendingSwitchStateIndex : -1;
    int pressedIndex = obj.id == pressedSwitchId ? pressedSwitchStateIndex : -1;
    return renderSwitch(obj, pendingIndex, pressedIndex);
  }
  if (obj.type == "switcher") {
    return renderTabControl(obj, pressedButtonId, pendingSwitchId, pendingSwitchStateIndex, pressedSwitchId, pressedSwitchStateIndex);
  }
  if (obj.type == "energy-diagram") return renderEnergyDiagram(obj);
  if (obj.type == "text-field") return renderTextField(obj);
  if (obj.type == "panel") {
    // Only ever meaningful as a tab-control's own child, handled by
    // renderTabControl() above - a stray top-level "panel" (malformed
    // data) draws nothing, matching the e-paper reference's identical no-op.
    return true;
  }

  Serial.printf("[ColorScreenRenderer] Object type \"%s\" not implemented yet, skipping (id=%s)\n",
                obj.type.c_str(), obj.id.c_str());
  return false;
}

void ColorScreenRenderer::hitTestWalk(const std::vector<ScreenObject>& objects, int16_t dx, int16_t dy,
                                       int16_t x, int16_t y, const ScreenObject*& treffer,
                                       int16_t& objX, int16_t& objY) const {
  // Dieselbe Sortierung wie renderScreen() und renderTabControl(), damit
  // "zuletzt gezeichnet" hier und dort dasselbe heisst.
  std::vector<const ScreenObject*> sorted;
  sorted.reserve(objects.size());
  for (const auto& o : objects) sorted.push_back(&o);
  std::sort(sorted.begin(), sorted.end(),
            [](const ScreenObject* a, const ScreenObject* b) { return zOrderLess(*a, *b); });

  for (const ScreenObject* o : sorted) {
    if (o->type == "switcher") {
      // Nur das sichtbare Panel. Wuerde hier ueber alle Panels gesucht,
      // traefe der Finger Schalter, die gar nicht auf dem Glas stehen.
      const ScreenObject* activePanel = getActivePanel(*o);
      if (activePanel != nullptr) {
        hitTestWalk(activePanel->children, dx + o->x, dy + o->y, x, y, treffer, objX, objY);
      }
      continue;
    }
    // Ein Panel ausserhalb eines tab-control zeichnet nichts (siehe
    // renderObject), also faengt es auch nichts.
    //
    // Ein Level mit Schreibtopic faengt ebenfalls: es ist bedienbar
    // (docs/2026-09-17-settable-level.md im Designer-Repo). Ohne Schreibtopic
    // nicht - ein Tank, den man nur liest, soll einen Finger nicht abfangen,
    // der in Wahrheit die Seite blaettern wollte.
    //
    // Ein Energiediagramm faengt nur auf einem Knoten mit Aktion (Vertrag
    // 2.8): anderswo soll der Finger blaettern koennen.
    const bool energy = o->type == "energy-diagram";
    if (!energy && !objecttypes::isSwitch(o->type) && o->type != "button" && !isSettableLevel(*o)) continue;

    const int16_t ox = dx + o->x;
    const int16_t oy = dy + o->y;
    if (x < ox || x >= ox + o->width) continue;
    if (y < oy || y >= oy + o->height) continue;
    if (energy && energyActionAt(*o, x - ox, y - oy) == nullptr) continue;
    treffer = o;
    objX = ox;
    objY = oy;
  }
}

namespace {
// Derselbe Gang wie hitTestWalk, nur nach Id statt nach Punkt: durch das
// sichtbare Panel eines tab-control hinein, mit aufaddiertem Versatz.
const ScreenObject* findByIdWalk(const std::vector<ScreenObject>& objects, const String& id, int16_t dx, int16_t dy,
                                  int16_t& objX, int16_t& objY,
                                  const std::function<const ScreenObject*(const ScreenObject&)>& activePanelOf) {
  for (const auto& o : objects) {
    if (o.id == id) {
      objX = dx + o.x;
      objY = dy + o.y;
      return &o;
    }
    if (o.type == "switcher") {
      const ScreenObject* panel = activePanelOf(o);
      if (panel != nullptr) {
        const ScreenObject* found =
            findByIdWalk(panel->children, id, dx + o.x, dy + o.y, objX, objY, activePanelOf);
        if (found != nullptr) return found;
      }
    }
  }
  return nullptr;
}
}  // namespace

const ScreenObject* ColorScreenRenderer::findObjectById(int screenIndex, const String& id, int16_t& objX,
                                                         int16_t& objY) const {
  if (!projectLoader_.isLoaded() || id.isEmpty()) return nullptr;
  const ProjectConfig& project = projectLoader_.getProject();
  const Screen* screenPtr = screenOrPopupAt(project, screenIndex);
  if (!screenPtr) return nullptr;
  objX = 0;
  objY = 0;
  return findByIdWalk(screenPtr->objects, id, 0, 0, objX, objY,
                      [this](const ScreenObject& tabControl) { return getActivePanel(tabControl); });
}

namespace {
// The first settable level among these objects, in their order, looking into
// a nested switcher's visible panel as findByIdWalk does.
const ScreenObject* firstSettableLevel(const std::vector<ScreenObject>& objects,
                                       const std::function<const ScreenObject*(const ScreenObject&)>& activePanelOf) {
  for (const auto& o : objects) {
    if (ColorScreenRenderer::isSettableLevel(o)) return &o;
    if (o.type == "switcher") {
      const ScreenObject* panel = activePanelOf(o);
      const ScreenObject* found = panel != nullptr ? firstSettableLevel(panel->children, activePanelOf) : nullptr;
      if (found != nullptr) return found;
    }
  }
  return nullptr;
}
}  // namespace

const ScreenObject* ColorScreenRenderer::adjustableLevel(int screenIndex, const String& targetId) const {
  int16_t objX = 0;
  int16_t objY = 0;
  const ScreenObject* target = findObjectById(screenIndex, targetId, objX, objY);
  if (target == nullptr) return nullptr;
  if (isSettableLevel(*target)) return target;
  if (target->type != "switcher") return nullptr;
  const ScreenObject* panel = getActivePanel(*target);
  if (panel == nullptr) return nullptr;
  return firstSettableLevel(panel->children, [this](const ScreenObject& s) { return getActivePanel(s); });
}

bool ColorScreenRenderer::adjustedLevelValue(const ScreenObject& level, const String& current, bool up, float& next) {
  float minValue = 0.0f;
  float maxValue = 100.0f;
  if (level.properties.calibrationPoints.size() >= 2) {
    minValue = maxValue = level.properties.calibrationPoints[0].value;
    for (const auto& p : level.properties.calibrationPoints) {
      minValue = fminf(minValue, p.value);
      maxValue = fmaxf(maxValue, p.value);
    }
  }
  const float step = level.properties.step > 0.0f ? level.properties.step : 1.0f;
  String trimmed = current;
  trimmed.trim();
  if (trimmed.isEmpty()) return false;
  const float from = trimmed.toFloat();
  const float snapped = roundf((from + (up ? step : -step)) / step) * step;
  // A step that does not divide the range tops out below the maximum, as a
  // finger does.
  const float highest = minValue + floorf((maxValue - minValue) / step + 1e-4f) * step;
  next = fminf(highest, fmaxf(minValue, snapped));
  return fabsf(next - from) > 1e-4f;
}

const ScreenObject* ColorScreenRenderer::hitTestTappable(int screenIndex, int16_t x, int16_t y,
                                                          int16_t& objX, int16_t& objY) const {
  if (!projectLoader_.isLoaded()) return nullptr;
  const ProjectConfig& project = projectLoader_.getProject();
  const Screen* screenPtr = screenOrPopupAt(project, screenIndex);
  if (!screenPtr) return nullptr;

  const ScreenObject* treffer = nullptr;
  objX = 0;
  objY = 0;
  hitTestWalk(screenPtr->objects, 0, 0, x, y, treffer, objX, objY);
  return treffer;
}

bool ColorScreenRenderer::renderScreen(int screenIndex, const String& pressedButtonId, const String& pendingSwitchId,
                                        int pendingSwitchStateIndex, const String& pressedSwitchId,
                                        int pressedSwitchStateIndex) {
  if (!projectLoader_.isLoaded()) return false;

  const ProjectConfig& project = projectLoader_.getProject();
  const Screen* screenPtr = screenOrPopupAt(project, screenIndex);
  if (!screenPtr) return false;

  const Screen& screen = *screenPtr;

  bool bgTransparent = false;
  uint16_t screenBg = parseHexColor(screen.backgroundColor.isEmpty() ? "#ffffff" : screen.backgroundColor, &bgTransparent);
  {
    RenderProfile::Scope s("background fill");
    canvas_->fillScreen(bgTransparent ? 0xFFFF : screenBg);
  }
  // Kept for the objects that need to know what they are drawn onto - an
  // arc-level mixes its anti-aliased edges into this when its own
  // background is transparent, which is the usual case for a ring.
  currentScreenBg_ = bgTransparent ? 0xFFFF : screenBg;
  currentScreenBgHex_ = bgTransparent || screen.backgroundColor.isEmpty() ? String("#ffffff") : screen.backgroundColor;

  std::vector<ScreenObject> sortedObjects = screen.objects;
  std::sort(sortedObjects.begin(), sortedObjects.end(), zOrderLess);

  for (const auto& obj : sortedObjects) {
    RenderProfile::Scope s(obj.type);
    renderObject(obj, pressedButtonId, pendingSwitchId, pendingSwitchStateIndex, pressedSwitchId, pressedSwitchStateIndex);
  }
  // Over everything, as it covers what lies under it (contract 2.7).
  renderNavigatorOver(screenIndex);
  // The pull tabs beside it (2.10), in the picture, so they slide with it.
  renderPullTabs(screenIndex);

  return true;
}

void ColorScreenRenderer::renderPullTabs(int screenIndex) {
  if (!projectLoader_.isLoaded()) return;
  const ProjectConfig& project = projectLoader_.getProject();
  if (screenIndex < 0 || screenIndex >= (int)project.screens.size()) return;
  const Screen& screen = project.screens[screenIndex];
  if (screen.pullTabColor.isEmpty()) return;
  const std::vector<screengraph::Edge> sides = screengraphproject::tabSidesAt(project, screenIndex);
  if (sides.empty()) return;
  const screengraph::Rect area = screengraphproject::contentAreaAt(project, screenIndex, canvas_->width(), canvas_->height());
  // A box as the designer draws one (contract 2.1): its round ends are a
  // corner radius every renderer already rasterises alike.
  for (screengraph::Edge side : sides) {
    const screengraph::Rect r = screengraph::tabRect(side, area, pullTabNudge_);
    ScreenObject tab;
    tab.type = "box";
    tab.id = "pull-tab";
    tab.x = r.x;
    tab.y = r.y;
    tab.width = r.width;
    tab.height = r.height;
    tab.zIndex = 0;
    tab.properties.fillColor = screen.pullTabColor;
    tab.properties.strokeColor = screen.pullTabColor;
    tab.properties.strokeWidth = 0;
    tab.properties.cornerRadius = screengraph::TAB_RADIUS;
    renderBox(tab);
  }
}

std::vector<screengraph::Rect> ColorScreenRenderer::pullTabRegions(int screenIndex) const {
  std::vector<screengraph::Rect> regions;
  if (!projectLoader_.isLoaded()) return regions;
  const ProjectConfig& project = projectLoader_.getProject();
  if (screenIndex < 0 || screenIndex >= (int)project.screens.size() || project.screens[screenIndex].pullTabColor.isEmpty()) return regions;
  const screengraph::Rect area = screengraphproject::contentAreaAt(project, screenIndex, canvas_->width(), canvas_->height());
  for (screengraph::Edge side : screengraphproject::tabSidesAt(project, screenIndex)) {
    const screengraph::Rect a = screengraph::tabRect(side, area, 0);
    const screengraph::Rect b = screengraph::tabRect(side, area, screengraph::TAB_NUDGE);
    const int x0 = std::min(a.x, b.x), y0 = std::min(a.y, b.y);
    regions.push_back({x0, y0, std::max(a.x + a.width, b.x + b.width) - x0, std::max(a.y + a.height, b.y + b.height) - y0});
  }
  return regions;
}

bool ColorScreenRenderer::navigatorLayout(int screenIndex, navigatorlayout::Layout& layout, const Navigator** navOut, int* activeOut) const {
  if (!projectLoader_.isLoaded()) return false;
  const ProjectConfig& project = projectLoader_.getProject();
  // A screen only: a popup shows none of its own.
  if (screenIndex < 0 || screenIndex >= (int)project.screens.size()) return false;
  const Screen& screen = project.screens[screenIndex];
  if (screen.navigatorId.isEmpty()) return false;
  for (const Navigator& nav : project.navigators) {
    if (nav.id != screen.navigatorId) continue;
    const navigatorlayout::Edge edge = navigatorlayout::edgeFrom(nav.edge.c_str());
    // Off the main row the way back is its one entry (2.10).
    const bool back = backArrow(nav, screen) >= 0;
    layout = navigatorlayout::barLayout(edge, navigatorlayout::stripOf(edge, nav.thickness, canvas_->width(), canvas_->height()),
                                        back ? 1 : (int)nav.entries.size(), nav.entryLength, nav.themeSwitchAt.c_str(),
                                        nav.themeSwitchLength);
    // The way back is as long as the entries' part of the strip (2.10).
    if (back) layout.entryLength = layout.length;
    if (navOut) *navOut = &nav;
    if (activeOut) {
      *activeOut = -1;
      for (size_t i = 0; !back && i < nav.entries.size(); i++)
        if (nav.entries[i].screenId == screen.id) *activeOut = (int)i;
    }
    return true;
  }
  return false;
}

int ColorScreenRenderer::backArrow(const Navigator& nav, const Screen& screen) {
  screengraph::Edge back;
  if (!screengraph::edgeFrom(screen.back.c_str(), back)) return -1;
  const int arrow = (int)screengraph::opposite(back);
  return nav.back[arrow].empty() ? -1 : arrow;
}

String ColorScreenRenderer::navigatorEntryScreenId(int screenIndex, int entry) const {
  navigatorlayout::Layout layout;
  const Navigator* nav = nullptr;
  if (!navigatorLayout(screenIndex, layout, &nav, nullptr) || entry < 0) return "";
  const Screen& screen = projectLoader_.getProject().screens[screenIndex];
  if (backArrow(*nav, screen) >= 0) {
    screengraph::Edge back;
    screengraph::edgeFrom(screen.back.c_str(), back);
    return entry == 0 ? screen.neighbours[(int)back] : String("");
  }
  return entry < (int)nav->entries.size() ? nav->entries[entry].screenId : String("");
}

int ColorScreenRenderer::effectiveNavigatorScroll(int screenIndex) const {
  navigatorlayout::Layout layout;
  int active = -1;
  if (!navigatorLayout(screenIndex, layout, nullptr, &active)) return 0;
  if (navigatorScroll_ >= 0) return navigatorlayout::clampScroll(layout, navigatorScroll_);
  return navigatorlayout::scrollToShow(layout, active < 0 ? 0 : active, 0);
}

bool ColorScreenRenderer::renderNavigatorStrip(int screenIndex, StripLook look) {
  navigatorlayout::Layout layout;
  if (!navigatorLayout(screenIndex, layout)) return false;
  stripLook_ = look;
  renderNavigatorOver(screenIndex);
  stripLook_ = StripLook::AsShown;
  return true;
}

bool ColorScreenRenderer::navigatorMarkerBox(int screenIndex, navigatorlayout::Rect& box, int& radius) const {
  navigatorlayout::Layout layout;
  const Navigator* nav = nullptr;
  if (!navigatorLayout(screenIndex, layout, &nav) || nav->entries.empty()) return false;
  for (const ScreenObject& o : nav->entries[0].active) {
    if (o.type != "box") continue;
    box = {o.x, o.y, o.width, o.height};
    radius = o.properties.cornerRadius < 0 ? 0 : o.properties.cornerRadius;
    return true;
  }
  return false;
}

void ColorScreenRenderer::renderNavigatorOver(int screenIndex) {
  navigatorlayout::Layout layout;
  const Navigator* nav = nullptr;
  int active = -1;
  if (!navigatorLayout(screenIndex, layout, &nav, &active)) return;

  // Its strip, cut to the region already in force (a partial redraw).
  const bool hadRegion = canvas_->regionActive();
  const int16_t px = canvas_->regionX(), py = canvas_->regionY(), pw = canvas_->regionW(), ph = canvas_->regionH();
  // A rectangle cut to the region already in force; false when nothing is left.
  auto clipTo = [&](const navigatorlayout::Rect& r, int& cx0, int& cy0, int& cx1, int& cy1) {
    cx0 = r.x;
    cy0 = r.y;
    cx1 = r.x + r.width;
    cy1 = r.y + r.height;
    if (hadRegion) {
      cx0 = std::max(cx0, (int)px);
      cy0 = std::max(cy0, (int)py);
      cx1 = std::min(cx1, (int)(px + pw));
      cy1 = std::min(cy1, (int)(py + ph));
    }
    return cx1 > cx0 && cy1 > cy0;
  };
  int x0, y0, x1, y1;
  if (!clipTo(layout.bar, x0, y0, x1, y1)) return;
  canvas_->setRegion(x0, y0, x1 - x0, y1 - y0);

  bool transparent = false;
  const uint16_t ground = parseHexColor(nav->backgroundColor.isEmpty() ? "#ffffff" : nav->backgroundColor, &transparent);
  canvas_->fillRect(x0, y0, x1 - x0, y1 - y0, transparent ? 0xFFFF : ground);
  // What an entry's arc or icon edges mix into: the strip's ground.
  const uint16_t screenBg = currentScreenBg_;
  const String screenBgHex = currentScreenBgHex_;
  currentScreenBg_ = transparent ? 0xFFFF : ground;
  currentScreenBgHex_ = transparent ? String("#ffffff") : nav->backgroundColor;

  // The entries within their part of the bar: they scroll, the switch's
  // place beside them stays.
  const int scroll = effectiveNavigatorScroll(screenIndex);
  int ex0, ey0, ex1, ey1;
  const bool entriesShown = clipTo(layout.strip, ex0, ey0, ex1, ey1);
  if (entriesShown) canvas_->setRegion(ex0, ey0, ex1 - ex0, ey1 - ey0);
  // Off the main row the way back alone (2.10).
  const int arrow = backArrow(*nav, projectLoader_.getProject().screens[screenIndex]);
  const int shownEntries = arrow >= 0 ? 1 : (int)nav->entries.size();
  // For the marker gliding (renderNavigatorStrip): none active, or every one
  // on the marker's colour - the box an entry's active look begins with,
  // stretched over the entries' part.
  const bool allActive = stripLook_ == StripLook::AllActive && arrow < 0;
  if (stripLook_ != StripLook::AsShown) active = -1;
  if (allActive && entriesShown && !nav->entries.empty()) {
    for (const ScreenObject& o : nav->entries[0].active) {
      if (o.type != "box") continue;
      ScreenObject ground = o;
      ground.x = layout.strip.x;
      ground.y = layout.strip.y;
      ground.width = layout.strip.width;
      ground.height = layout.strip.height;
      ground.properties.cornerRadius = 0;
      renderObject(ground, "", "", -1, "", -1);
      break;
    }
  }
  for (int i = 0; entriesShown && i < shownEntries; i++) {
    const navigatorlayout::Rect r = navigatorlayout::entryRect(layout, i, scroll);
    if (r.x >= ex1 || r.y >= ey1 || r.x + r.width <= ex0 || r.y + r.height <= ey0) continue;
    const SpiramObjects& source = arrow >= 0 ? nav->back[arrow] : (allActive || i == active) ? nav->entries[i].active : nav->entries[i].normal;
    std::vector<ScreenObject> objects(source.begin(), source.end());
    std::sort(objects.begin(), objects.end(), zOrderLess);
    for (ScreenObject obj : objects) {
      obj.x += r.x;
      obj.y += r.y;
      renderObject(obj, "", "", -1, "", -1);
    }
  }

  // The light/dark switch in its place (2.7, generation 1.6).
  int sx0, sy0, sx1, sy1;
  if (layout.hasThemeSwitch && clipTo(layout.themeSwitch, sx0, sy0, sx1, sy1)) {
    canvas_->setRegion(sx0, sy0, sx1 - sx0, sy1 - sy0);
    std::vector<ScreenObject> objects(nav->themeSwitchObjects.begin(), nav->themeSwitchObjects.end());
    std::sort(objects.begin(), objects.end(), zOrderLess);
    for (ScreenObject obj : objects) {
      obj.x += layout.themeSwitch.x;
      obj.y += layout.themeSwitch.y;
      renderObject(obj, "", "", -1, "", -1);
    }
  }
  canvas_->setRegion(x0, y0, x1 - x0, y1 - y0);

  // The line towards the screen, over the entries (2.7, 2026-10-10): so the
  // strip reads as a bar, not as one button on the screen's ground.
  if (!nav->borderColor.isEmpty()) {
    bool noLine = false;
    const uint16_t line = parseHexColor(nav->borderColor, &noLine);
    if (!noLine) {
      const navigatorlayout::Rect d = navigatorlayout::dividerRect(layout.edge, layout.bar);
      canvas_->fillRect(d.x, d.y, d.width, d.height, line);
    }
  }

  currentScreenBg_ = screenBg;
  currentScreenBgHex_ = screenBgHex;
  if (hadRegion) canvas_->setRegion(px, py, pw, ph);
  else canvas_->clearRegion();
}

// A popup over what the canvas holds now (designer docs/device-contract.md
// 2.5): no fillScreen - the screen underneath stays, the board has already
// set it back outside the fence - then the popup's background, clipped to the
// fence's shape, then its objects where they are on the display, sorted as a
// screen's are.
bool ColorScreenRenderer::renderPopup(int popupIndex, int borderPx, const String& pressedButtonId,
                                      const String& pendingSwitchId, int pendingSwitchStateIndex,
                                      const String& pressedSwitchId, int pressedSwitchStateIndex) {
  if (!projectLoader_.isLoaded()) return false;
  const ProjectConfig& project = projectLoader_.getProject();
  if (!isPopupIndex(project, popupIndex) || !project.popupFence.present) return false;
  const Screen& popup = *screenOrPopupAt(project, popupIndex);
  const PopupFence& fence = project.popupFence;

  bool bgTransparent = false;
  uint16_t bg = parseHexColor(popup.backgroundColor.isEmpty() ? "#ffffff" : popup.backgroundColor, &bgTransparent);
  currentScreenBg_ = bgTransparent ? 0xFFFF : bg;
  currentScreenBgHex_ = bgTransparent || popup.backgroundColor.isEmpty() ? String("#ffffff") : popup.backgroundColor;
  if (fence.circle) {
    // Adafruit GFX's circle of radius r is 2r+1 wide: the same pixels
    // PopupFence::contains() holds for an even diameter.
    canvas_->fillCircle(fence.x + fence.width / 2, fence.y + fence.height / 2, fence.width / 2, currentScreenBg_);
  } else {
    canvas_->fillRect(fence.x, fence.y, fence.width, fence.height, currentScreenBg_);
  }
  drawPopupBorder(popup, borderPx);

  std::vector<ScreenObject> sortedObjects = popup.objects;
  std::sort(sortedObjects.begin(), sortedObjects.end(), zOrderLess);
  for (const auto& obj : sortedObjects) {
    renderObject(obj, pressedButtonId, pendingSwitchId, pendingSwitchStateIndex, pressedSwitchId, pressedSwitchStateIndex);
  }
  drawPopupCloseBadge(popup);
  return true;
}

PopupCloseBadge ColorScreenRenderer::popupCloseBadge() const {
  if (!projectLoader_.isLoaded()) return PopupCloseBadge();
  return popupCloseBadgeFor(projectLoader_.getProject().popupFence, popupCloseBadgeRadius_);
}

// A dark disc with a white X, over everything else - an object under it is
// the author's to move. Dark whatever the theme: in the theme's outline it was
// pale grey on the glass, and on e-ink, with no scrim round the window, hardly
// there (the user, 2026-10-07). The X's strokes as rows of small discs, for
// round ends without anti-aliasing.
void ColorScreenRenderer::drawPopupCloseBadge(const Screen& popup) {
  (void)popup;
  const PopupCloseBadge b = popupCloseBadge();
  if (!b.present) return;
  const uint16_t disc = parseHexColor(POPUP_CLOSE_BADGE_DISC);
  const uint16_t cross = parseHexColor(POPUP_CLOSE_BADGE_CROSS);
  canvas_->fillCircle(b.cx, b.cy, b.radius, disc);
  const int arm = b.radius * 2 / 5;
  const int pen = b.radius / 10 > 1 ? b.radius / 10 : 1;
  for (int i = -arm; i <= arm; i++) {
    canvas_->fillCircle(b.cx + i, b.cy + i, pen, cross);
    canvas_->fillCircle(b.cx + i, b.cy - i, pen, cross);
  }
}

// The fence's edge in the popup's border colour (the theme's outline), inward
// from the edge so it never reaches the screen underneath.
void ColorScreenRenderer::drawPopupBorder(const Screen& popup, int borderPx) {
  const PopupFence& fence = projectLoader_.getProject().popupFence;
  if (borderPx <= 0 || popup.borderColor.isEmpty()) return;
  bool transparent = false;
  const uint16_t color = parseHexColor(popup.borderColor, &transparent);
  if (transparent) return;
  for (int i = 0; i < borderPx; i++) {
    if (fence.circle) {
      canvas_->drawCircle(fence.x + fence.width / 2, fence.y + fence.height / 2, fence.width / 2 - i, color);
    } else {
      canvas_->drawRect(fence.x + i, fence.y + i, fence.width - 2 * i, fence.height - 2 * i, color);
    }
  }
}

void ColorScreenRenderer::dimCanvas() {
  uint16_t* buffer = canvas_->getBuffer();
  const size_t n = (size_t)canvas_->width() * canvas_->height();
  for (size_t i = 0; i < n; i++) buffer[i] = halveRgb565(buffer[i]);
}

// Halves every pixel outside the fence: a black scrim at 50 %, what an RGB
// board sets the screen underneath an open popup back with. Row by row, the
// inside of a circle as one span, so the cost is one pass over the canvas.
void ColorScreenRenderer::dimOutsidePopupFence() {
  if (!projectLoader_.isLoaded()) return;
  const PopupFence& fence = projectLoader_.getProject().popupFence;
  if (!fence.present) return;
  uint16_t* buffer = canvas_->getBuffer();
  const int w = canvas_->width();
  const int h = canvas_->height();
  for (int y = 0; y < h; y++) {
    int from = 0, to = -1;  // the row's inside, [from, to]
    if (fence.circle) {
      const long r2 = (long)fence.width * fence.width;  // (2r)^2
      const long dy = 2L * y - (2L * fence.y + fence.height);
      const long rest = r2 - dy * dy;
      if (rest >= 0) {
        // Widest 2*dx with (2*dx)^2 <= rest, then back to pixels.
        long half = (long)sqrtf((float)rest);
        while (half * half > rest) half--;
        while ((half + 1) * (half + 1) <= rest) half++;
        const long cx2 = 2L * fence.x + fence.width;
        from = (int)((cx2 - half + 1) / 2);
        to = (int)((cx2 + half) / 2);
      }
    } else if (y >= fence.y && y < fence.y + fence.height) {
      from = fence.x;
      to = fence.x + fence.width - 1;
    }
    uint16_t* row = buffer + (size_t)y * w;
    for (int x = 0; x < w; x++) {
      if (x >= from && x <= to) {
        x = to;
        continue;
      }
      row[x] = halveRgb565(row[x]);
    }
  }
}

// Ausschnittfassung. Teilt sich mit renderScreen() bewusst NICHT nur die
// Objektliste, sondern auch deren Sortierung und den Hintergrund - jede
// Abweichung waere ein Pixelunterschied, und genau darauf pruefen die
// HIL-Suiten.
bool ColorScreenRenderer::renderScreenRegion(int screenIndex, int16_t rx, int16_t ry, int16_t rw, int16_t rh,
                                             const String& pressedButtonId, const String& pendingSwitchId,
                                             int pendingSwitchStateIndex, const String& pressedSwitchId,
                                             int pressedSwitchStateIndex) {
  if (!projectLoader_.isLoaded()) return false;
  const ProjectConfig& project = projectLoader_.getProject();
  const Screen* screenPtr = screenOrPopupAt(project, screenIndex);
  if (!screenPtr) return false;
  if (rw <= 0 || rh <= 0) return false;

  const Screen& screen = *screenPtr;

  bool bgTransparent = false;
  uint16_t screenBg = parseHexColor(screen.backgroundColor.isEmpty() ? "#ffffff" : screen.backgroundColor, &bgTransparent);
  currentScreenBg_ = bgTransparent ? 0xFFFF : screenBg;
  currentScreenBgHex_ = bgTransparent || screen.backgroundColor.isEmpty() ? String("#ffffff") : screen.backgroundColor;

  canvas_->setRegion(rx, ry, rw, rh);
  canvas_->fillRect(rx, ry, rw, rh, currentScreenBg_);

  std::vector<ScreenObject> sortedObjects = screen.objects;
  std::sort(sortedObjects.begin(), sortedObjects.end(), zOrderLess);
  // Was den Ausschnitt nicht beruehrt, wird gar nicht erst gezeichnet.
  //
  // Nur zu clippen genuegt hier nicht: renderObject() laedt fuer Icons und
  // Schalter-Symbole die Bilddatei aus dem Dateisystem, bevor ueberhaupt ein
  // Pixel geschrieben wird - der Clip unterdrueckt das Schreiben, nicht das
  // Lesen. Gemessen kosteten drei Ausschnitte auf einem Screen mit vier
  // Symbolen dadurch 410ms, also mehr als das Vollbild vorher.
  //
  // Der Rand faengt Objekte ab, die etwas ueber ihre eigenen Masse hinaus
  // zeichnen - Text mit Unterlaenge etwa.
  const int16_t RAND = 8;
  for (const auto& obj : sortedObjects) {
    if (obj.x + obj.width + RAND <= rx || obj.x - RAND >= rx + rw) continue;
    if (obj.y + obj.height + RAND <= ry || obj.y - RAND >= ry + rh) continue;
    renderObject(obj, pressedButtonId, pendingSwitchId, pendingSwitchStateIndex, pressedSwitchId,
                 pressedSwitchStateIndex);
  }
  // A popup's close button lies over its objects; one redrawn under it would
  // otherwise cover it.
  if (isPopupIndex(project, screenIndex)) {
    const PopupCloseBadge b = popupCloseBadge();
    if (b.present && b.cx + b.radius >= rx && b.cx - b.radius < rx + rw && b.cy + b.radius >= ry &&
        b.cy - b.radius < ry + rh) {
      drawPopupCloseBadge(screen);
    }
  }

  // The navigator over the screen here too, cut to the region (2.7), and
  // the pull tabs beside it (2.10).
  renderNavigatorOver(screenIndex);
  renderPullTabs(screenIndex);
  canvas_->clearRegion();
  return true;
}

// Das Rechteck, das sich beim Schritt von oldValue auf newValue aendert.
//
// Statt die Sektorgeometrie nachzubauen wird der geaenderte Winkelbereich
// abgetastet: fuer eine Reihe von Winkeln zwischen altem und neuem Bogenende
// die Punkte auf Aussen- und Innenradius nehmen und die Huelle bilden. Ein
// Rand von ein paar Pixeln deckt die Kantenglaettung und die Schrittweite der
// Abtastung ab - lieber ein paar Pixel zu viel neu zeichnen als einen zu
// wenig, denn ein fehlender waere ein Rest des alten Bildes.
// Das Rechteck der Zahl in der Mitte. Die Schriftmasse werden hier genauso
// berechnet wie beim Zeichnen, und die Huelle beider Texte genommen - "100"
// ist breiter als "40", also muss der breitere Stand mit weg.
bool ColorScreenRenderer::arcLevelTextRect(const ScreenObject& obj, const String& oldValue, const String& newValue,
                                           int16_t* outX, int16_t* outY, int16_t* outW, int16_t* outH) {
  if (obj.properties.displayValue == "none") return false;

  const BdfFont* font = fontFor(obj.properties.fontId);
  if (font == nullptr) return false;
  const int16_t fontAscent = getFontAscentById(obj.properties.fontId);
  const int16_t fontDescent = getFontDescentById(obj.properties.fontId);
  const int16_t fontHeight = fontAscent + fontDescent;

  int16_t breiteste = 0;
  for (int pass = 0; pass < 2; pass++) {
    const String& roh = pass == 0 ? oldValue : newValue;
    String text;
    if (obj.properties.displayValue == "percentage") {
      text = String((int)(interpolateCalibration(roh.toFloat(), obj.properties.calibrationPoints) + 0.5f)) + "%";
    } else {
      text = roh;
    }
    const int16_t w = measureTrueTextWidth(text, font);
    if (w > breiteste) breiteste = w;
  }
  if (breiteste <= 0) return false;

  // Mittig wie beim Zeichnen; ein Rand deckt Rundung und Ueberhang ab.
  const int32_t PAD = 4;
  const int32_t x = obj.x + jsRound((obj.width - breiteste) / 2.0f) - PAD;
  const int32_t y = obj.y + jsRound((obj.height - fontHeight) / 2.0f) - PAD;
  *outX = (int16_t)(x < obj.x ? obj.x : x);
  *outY = (int16_t)(y < obj.y ? obj.y : y);
  *outW = (int16_t)(breiteste + 2 * PAD);
  *outH = (int16_t)(fontHeight + 2 * PAD);
  return true;
}

bool ColorScreenRenderer::arcLevelCentreRect(const ScreenObject& obj, int16_t* outX, int16_t* outY, int16_t* outW,
                                             int16_t* outH) {
  if (obj.properties.displayValue == "none") return false;
  const int32_t size = obj.width < obj.height ? obj.width : obj.height;
  if (size <= 0) return false;
  const int32_t thickness = arcThicknessFor(obj, size);
  const int32_t inset = arcInsetFor(obj, size, thickness);
  const int32_t rInner = size / 2 - inset - thickness;
  if (rInner <= 0) return false;
  if (fontFor(obj.properties.fontId) == nullptr) return false;
  const int32_t line = getFontAscentById(obj.properties.fontId) + getFontDescentById(obj.properties.fontId);
  int32_t block = line;
  const String subId = levelSubFontId(obj);
  if (fontFor(subId) != nullptr) block += getFontAscentById(subId) + getFontDescentById(subId) + kArcSubGap;
  // A margin for rounding and for glyphs that reach past their line.
  const int32_t PAD = 4;
  int32_t h = block + 2 * PAD;
  if (h > 2 * rInner) h = 2 * rInner;
  // Centred on the object, as renderArcLevel centres the numbers.
  const int32_t cx = obj.x + obj.width / 2;
  const int32_t cy = obj.y + obj.height / 2;
  *outX = (int16_t)(cx - rInner);
  *outY = (int16_t)(cy - h / 2 - 1);
  *outW = (int16_t)(2 * rInner);
  *outH = (int16_t)(h + 2);
  return true;
}

int ColorScreenRenderer::levelDragRects(int screenIndex, const String& markerTopic, const String& oldValue,
                                        const String& newValue, int16_t* rx, int16_t* ry, int16_t* rw, int16_t* rh,
                                        int max) {
  if (!projectLoader_.isLoaded() || markerTopic.isEmpty()) return -1;
  const ProjectConfig& project = projectLoader_.getProject();
  const Screen* screenPtr = screenOrPopupAt(project, screenIndex);
  if (!screenPtr) return -1;
  int count = 0;
  for (const auto& o : screenPtr->objects) {
    // Nested objects stand relative to their parent: markerTopicBounds knows
    // how to walk them, this does not try.
    if (o.type == "switcher") return -1;
    const String topic = o.properties.setpointTopic.isEmpty() ? o.properties.topic : o.properties.setpointTopic;
    if (!objecttypes::isAnyLevel(o.type) || topic != markerTopic) continue;
    if (objecttypes::isArc(o.type) && !oldValue.isEmpty() && !newValue.isEmpty()) {
      if (count + 2 > max) return -1;
      if (!arcLevelDirtyRect(o, oldValue, newValue, &rx[count], &ry[count], &rw[count], &rh[count])) return -1;
      count++;
      if (arcLevelCentreRect(o, &rx[count], &ry[count], &rw[count], &rh[count])) count++;
      continue;
    }
    if (count + 2 > max) return -1;
    if (!oldValue.isEmpty() && !newValue.isEmpty()) {
      const int n = barLevelDragRects(o, oldValue, newValue, &rx[count], &ry[count], &rw[count], &rh[count]);
      if (n > 0) {
        count += n;
        continue;
      }
    }
    rx[count] = o.x;
    ry[count] = o.y;
    rw[count] = o.width;
    rh[count] = o.height;
    count++;
  }
  return count;
}

int ColorScreenRenderer::barLevelDragRects(const ScreenObject& obj, const String& oldValue, const String& newValue,
                                           int16_t* rx, int16_t* ry, int16_t* rw, int16_t* rh) {
  if (!objecttypes::isLevel(obj.type)) return -1;
  const LevelLayout lay = levelshape::layout(obj, levelMetrics(obj));
  if (!lay.track.valid()) return -1;
  const bool vertical = levelshape::isVertical(obj);
  const bool fromEnd = levelshape::fillsFromEnd(obj);

  // Everything that can move between the two values, as a span along the bar.
  int lo = 1 << 30, hi = -(1 << 30);
  auto take = [&](int a, int b) {
    if (a < lo) lo = a;
    if (b > hi) hi = b;
  };
  for (int pass = 0; pass < 2; pass++) {
    float percent = interpolateCalibration((pass == 0 ? oldValue : newValue).toFloat(),
                                           obj.properties.calibrationPoints);
    if (percent < 0.0f) percent = 0.0f;
    if (percent > 100.0f) percent = 100.0f;
    const int edge = levelshape::edgeFor(lay.track, vertical, fromEnd, percent);
    take(edge, edge);
    const LevelRect handle = levelshape::handleRect(obj, lay, percent);
    if (handle.valid()) {
      if (vertical) take(handle.y, handle.y + handle.h);
      else take(handle.x, handle.x + handle.w);
    }
    LevelRect pointer;
    bool pointsLeft = false;
    if (levelshape::pointerRect(obj, lay, percent, pointer, pointsLeft)) {
      if (vertical) take(pointer.y, pointer.y + pointer.h);
      else take(pointer.x, pointer.x + pointer.w);
    }
  }
  // The glow round the fill's end, the handle's gap and the soft edges.
  const int pad = levelglow::glowPx(obj.properties.glow) + (vertical ? lay.track.w : lay.track.h) / 2 + 4;
  lo -= pad;
  hi += pad;

  int count = 0;
  if (vertical) {
    const int y0 = lo < obj.y ? obj.y : lo;
    const int y1 = hi > obj.y + obj.height ? obj.y + obj.height : hi;
    if (y1 <= y0) return -1;
    rx[count] = obj.x;
    ry[count] = (int16_t)y0;
    rw[count] = obj.width;
    rh[count] = (int16_t)(y1 - y0);
  } else {
    const int x0 = lo < obj.x ? obj.x : lo;
    const int x1 = hi > obj.x + obj.width ? obj.x + obj.width : hi;
    if (x1 <= x0) return -1;
    rx[count] = (int16_t)x0;
    ry[count] = obj.y;
    rw[count] = (int16_t)(x1 - x0);
    rh[count] = obj.height;
  }
  count++;

  // The number's own column.
  const LevelRect* text = lay.hasValue ? &lay.value : nullptr;
  if (text != nullptr && text->valid() && levelshape::showsNumber(obj)) {
    rx[count] = (int16_t)text->x;
    ry[count] = (int16_t)text->y;
    rw[count] = (int16_t)text->w;
    rh[count] = (int16_t)text->h;
    count++;
  }
  return count;
}

bool ColorScreenRenderer::arcLevelDirtyRect(const ScreenObject& obj, const String& oldValue, const String& newValue,
                                            int16_t* outX, int16_t* outY, int16_t* outW, int16_t* outH) {
  if (!objecttypes::isArc(obj.type)) return false;
  if (oldValue.isEmpty() || newValue.isEmpty()) return false;

  const int32_t size = obj.width < obj.height ? obj.width : obj.height;
  if (size <= 0) return false;

  const float pctOld = interpolateCalibration(oldValue.toFloat(), obj.properties.calibrationPoints);
  const float pctNew = interpolateCalibration(newValue.toFloat(), obj.properties.calibrationPoints);
  const float lo = pctOld < pctNew ? pctOld : pctNew;
  const float hi = pctOld < pctNew ? pctNew : pctOld;

  // Wortgleich zu renderArcLevel hergeleitet - eine eigene Fassung waere
  // genau die Copy-paste-Falle, die diesen Flow schon einmal erwischt hat.
  const int32_t minA = ((obj.properties.minAngle % 360) + 360) % 360;
  const int32_t maxA = ((obj.properties.maxAngle % 360) + 360) % 360;
  const bool counterClockwise = obj.properties.arcDirection == "ccw";
  const int32_t startDeg = counterClockwise ? maxA : minA;
  int32_t spanDeg = counterClockwise ? ((minA - maxA + 360) % 360) : ((maxA - minA + 360) % 360);
  if (spanDeg == 0) spanDeg = 360;
  const int32_t start64 = startDeg * ArcRaster::ANGLE_SCALE;
  const int32_t sweep64 = spanDeg * ArcRaster::ANGLE_SCALE;

  const int32_t centre = size / 2;
  // Von renderArcLevel geliehen, nicht nachgebaut: seit 2026-09-22 rueckt der
  // Ring um den Platz des Griffs nach innen, und eine Huelle am alten
  // Aussenradius laesst die Kante, die sich wirklich bewegt hat, stehen.
  const int32_t thickness = arcThicknessFor(obj, size);
  const int32_t inset = arcInsetFor(obj, size, thickness);
  const int32_t rOuter = size / 2 - inset;
  const int32_t rInner = rOuter - thickness > 0 ? rOuter - thickness : 0;

  int32_t minX = size, minY = size, maxX = -1, maxY = -1;
  const int STEPS = 24;
  for (int i = 0; i <= STEPS; i++) {
    const float pct = lo + (hi - lo) * (float)i / (float)STEPS;
    const int32_t at = ArcRaster::sweepForPercent(sweep64, pct);
    // Ein Gegenuhrzeiger-Zifferblatt fuellt vom anderen Ende seines Sektors
    // (fillStart64 in renderArcLevel), also liegt seine bewegte Kante dort -
    // ohne das lag die Huelle auf der falschen Seite der Skala.
    const int32_t angle64 = counterClockwise ? start64 + sweep64 - at : start64 + at;
    int32_t dx = 0, dy = 0;
    // ACHTUNG: direction() liefert die Richtung in der Skala der Sinustabelle
    // (ARC_SIN_SCALE = 16384), NICHT in ANGLE_SCALE (64). Mit der falschen
    // Konstante kollabierte die Huelle auf wenige Pixel, der Bogen wurde also
    // gar nicht neu gezeichnet - schnell, aber falsch.
    ArcRaster::direction(angle64, &dx, &dy);
    for (int pass = 0; pass < 2; pass++) {
      const int32_t r = pass == 0 ? rOuter : rInner;
      const int32_t x = centre + (dx * r) / ARC_SIN_SCALE;
      const int32_t y = centre + (dy * r) / ARC_SIN_SCALE;
      if (x < minX) minX = x;
      if (y < minY) minY = y;
      if (x > maxX) maxX = x;
      if (y > maxY) maxY = y;
    }
  }
  if (maxX < minX || maxY < minY) return false;

  // Die Zahl in der Mitte bekommt ihr EIGENES Rechteck (arcLevelTextRect).
  // Sie hier mitzunehmen hiess, die Huelle aus Bogenabschnitt und Bildmitte
  // zu bilden - und die ist fast so gross wie das ganze Bild. Zwei kleine
  // Rechtecke sind billiger als ein grosses, das beide umschliesst.

  // Der Rand deckt Rundung und Ueberhang ab, und seit 2026-09-22 zwei Dinge
  // mehr, die ueber die Bahn des Mittelkreises hinausreichen: die runde Kappe
  // am Ende der Skala (eine halbe Dicke) und der Griff, der quer auf dem Band
  // liegt und auf beiden Seiten aus ihm herausschaut. Seine halbe Laenge kommt
  // aus handleBand() selbst - die Klemmungen dort ein zweites Mal
  // hinzuschreiben waere wieder die Copy-paste-Falle.
  int32_t PAD = 4 + thickness / 2;
  if (!obj.properties.writeTopic.isEmpty() || !obj.properties.setpointTopic.isEmpty()) {
    const int32_t half = ArcRaster::handleBand(size, thickness, inset, start64, sweep64).halfLength;
    PAD += (half + ArcRaster::SUBPIXEL_SCALE - 1) / ArcRaster::SUBPIXEL_SCALE;
  }
  // The glow round the fill's moving end reaches as far again.
  PAD += levelglow::glowPx(obj.properties.glow);
  minX -= PAD; minY -= PAD; maxX += PAD; maxY += PAD;
  if (minX < 0) minX = 0;
  if (minY < 0) minY = 0;
  if (maxX > size - 1) maxX = size - 1;
  if (maxY > size - 1) maxY = size - 1;

  *outX = (int16_t)(obj.x + minX);
  *outY = (int16_t)(obj.y + minY);
  *outW = (int16_t)(maxX - minX + 1);
  *outH = (int16_t)(maxY - minY + 1);
  return true;
}

