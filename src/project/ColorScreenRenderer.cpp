#include "ColorScreenRenderer.h"
#include "RenderProfile.h"
#include <esp_cpu.h>
#include "ArcRaster.h"
#include "ArcSinTable.h"  // ARC_SIN_SCALE - Skala von ArcRaster::direction()
#include <algorithm>

// Matches JavaScript's Math.round() exactly, including its tie-breaking
// direction (halves round toward +infinity) - see MqttEPaperDisplay2's
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
void ColorScreenRenderer::setAskedValue(const String& objectId, const String& value) {
  askedObjectId_ = objectId;
  askedValue_ = value;
}

void ColorScreenRenderer::clearAskedValue() {
  askedObjectId_ = "";
  askedValue_ = "";
}

bool ColorScreenRenderer::isSettableLevel(const ScreenObject& obj) {
  return (obj.type == "level-indicator" || obj.type == "arc-level") && !obj.properties.writeTopic.isEmpty();
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
  if (obj.type == "arc-level") {
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
  // - matches the designer's draw order exactly, see MqttEPaperDisplay2's
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

bool ColorScreenRenderer::renderLabel(const ScreenObject& obj) {
  drawTextBox(obj, obj.properties.text, true);
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
// MqttEPaperDisplay2's ScreenRenderer::renderMqttDataLine().
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
// arc between tangent points - see MqttEPaperDisplay2's ScreenRenderer::
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
// MqttEPaperDisplay2's ScreenRenderer::fillThickLine() for the full
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
  if (screenIndex < 0 || screenIndex >= (int)project.screens.size()) return "no such screen\n";
  const Screen& screen = project.screens[screenIndex];

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

// Same bar/text layout math as MqttEPaperDisplay2's renderLevelIndicator()
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
bool ColorScreenRenderer::renderLevelIndicator(const ScreenObject& obj) {
  String valueStr = projectLoader_.getTopicValue(obj.properties.topic);
  float value = valueStr.isEmpty() ? 0.0f : valueStr.toFloat();

  float fillPercent = interpolateCalibration(value, obj.properties.calibrationPoints);
  if (fillPercent < 0) fillPercent = 0;
  if (fillPercent > 100) fillPercent = 100;

  bool bgTransparent = false;
  uint16_t bgColor = parseHexColor(obj.properties.backgroundColor, &bgTransparent);
  uint16_t fillColor = parseHexColor(obj.properties.fillColor);
  bool borderTransparent = false;
  uint16_t borderColor = parseHexColor(obj.properties.borderColor, &borderTransparent);

  if (!bgTransparent) {
    canvas_->fillRect(obj.x, obj.y, obj.width, obj.height, bgColor);
  }
  if (!borderTransparent) {
    canvas_->drawRect(obj.x, obj.y, obj.width, obj.height, borderColor);
  }

  // No value yet: the frame, and neither bar nor text - an empty bar would
  // claim an empty tank (render-level-indicator.ts, the same rule).
  String trimmedValue = valueStr;
  trimmedValue.trim();
  if (trimmedValue.isEmpty()) return true;

  const int padding = 4;
  int innerX = obj.x + padding;
  int innerY = obj.y + padding;
  int innerWidth = obj.width - padding * 2;
  int innerHeight = obj.height - padding * 2;

  int fillWidth = innerWidth, fillHeight = innerHeight, fillX = innerX, fillY = innerY;
  if (obj.properties.barDirection == "left-to-right") {
    fillWidth = (innerWidth * fillPercent) / 100;
  } else if (obj.properties.barDirection == "right-to-left") {
    fillWidth = (innerWidth * fillPercent) / 100;
    fillX = innerX + innerWidth - fillWidth;
  } else if (obj.properties.barDirection == "bottom-to-top") {
    fillHeight = (innerHeight * fillPercent) / 100;
    fillY = innerY + innerHeight - fillHeight;
  } else if (obj.properties.barDirection == "top-to-bottom") {
    fillHeight = (innerHeight * fillPercent) / 100;
  }
  int barX = fillX, barY = fillY, barWidth = fillWidth, barHeight = fillHeight;

  // Der Sollwert-Marker, gezeichnet wie im Designer: ueber der Fuellung und
  // unter dem zweiten Textdurchgang (render-level-indicator.ts). Vor dem Text
  // gezeichnet lag er unter den Ziffern - 19 Pixel Unterschied im
  // Conformance-Lauf, weil der Designer ihn darueber legt (2026-09-17).
  auto drawSetpointMarker = [&]() {
    // Der Sollwert-Marker: was angefordert wurde, neben dem, was gemessen wird -
    // dieselbe zweite Bindung, die der Bogen schon immer hatte
    // (docs/2026-09-17-settable-level.md im Designer-Repo, Entscheidung 6c).
    // Dieselbe Innenbox und dieselbe Abschneidung wie die Fuellung, damit ein
    // Marker auf demselben Wert auf deren Kante sitzt und nicht daneben -
    // wortgleich zu computeMarkerRect() in render-level-indicator.ts.
    // Der Marker zeigt, was angefordert wurde: das Sollwert-Topic, wo es eines
    // gibt, sonst was ein Finger an diesem Objekt gerade gesetzt hat
    // (Entscheidung 6c). Die Fuellung bleibt der gemessene Wert - laufen beide
    // zusammen, ist der Befehl angekommen.
    // Was ein Finger gerade angefordert hat, gewinnt: er bedient ja genau
    // diesen Sollwert, und der alte Stand darf den Wunsch nicht ueberdecken.
    // Verworfen wird die Anforderung, sobald die Anlage auf dem Sollwert-Topic
    // antwortet - oder, wo es keines gibt, auf dem Lese-Topic (main.cpp).
    String setpointStr = obj.properties.setpointTopic.isEmpty()
                             ? String("")
                             : projectLoader_.getTopicValue(obj.properties.setpointTopic);
    if (!askedObjectId_.isEmpty() && obj.id == askedObjectId_ && !askedValue_.isEmpty()) {
      setpointStr = askedValue_;
    }
    {
      String trimmedSetpoint = setpointStr;
      trimmedSetpoint.trim();
      if (!trimmedSetpoint.isEmpty()) {
        float setpointPercent = interpolateCalibration(setpointStr.toFloat(), obj.properties.calibrationPoints);
        if (setpointPercent < 0.0f) setpointPercent = 0.0f;
        if (setpointPercent > 100.0f) setpointPercent = 100.0f;

        int thickness = (int)lroundf(obj.properties.markerWidth <= 0 ? 4.0f : (float)obj.properties.markerWidth);
        if (thickness < 1) thickness = 1;
        const bool vertical =
            obj.properties.barDirection == "bottom-to-top" || obj.properties.barDirection == "top-to-bottom";

        bool markerTransparent = false;
        uint16_t markerColor = parseHexColor(
            obj.properties.markerColor.isEmpty() ? "#ffffff" : obj.properties.markerColor, &markerTransparent);

        if (!markerTransparent) {
          if (vertical) {
            const int filled = (innerHeight * (int)setpointPercent) / 100;
            const int edge = obj.properties.barDirection == "bottom-to-top" ? innerY + innerHeight - filled
                                                                            : innerY + filled;
            int markerY = edge - thickness / 2;
            if (markerY < innerY) markerY = innerY;
            if (markerY > innerY + innerHeight - thickness) markerY = innerY + innerHeight - thickness;
            const int markerH = thickness < innerHeight ? thickness : innerHeight;
            if (markerH > 0 && innerWidth > 0) canvas_->fillRect(innerX, markerY, innerWidth, markerH, markerColor);
          } else {
            const int filled = (innerWidth * (int)setpointPercent) / 100;
            const int edge =
                obj.properties.barDirection == "right-to-left" ? innerX + innerWidth - filled : innerX + filled;
            int markerX = edge - thickness / 2;
            if (markerX < innerX) markerX = innerX;
            if (markerX > innerX + innerWidth - thickness) markerX = innerX + innerWidth - thickness;
            const int markerW = thickness < innerWidth ? thickness : innerWidth;
            if (markerW > 0 && innerHeight > 0) canvas_->fillRect(markerX, innerY, markerW, innerHeight, markerColor);
          }
        }
      }
    }
  };

  // Der Balken selbst. Bis 2026-09-17 kehrte diese Funktion bei
  // displayValue "none" zurueck, BEVOR sie ihn zeichnete - ein Balken ohne
  // Zahl blieb auf dem Geraet leer, waehrend der Designer ihn zeichnet. Kein
  // Conformance-Fall hat das gesehen, weil das Muster immer Prozent anzeigt.
  auto drawBar = [&]() {
    if (barWidth > 0 && barHeight > 0) canvas_->fillRect(barX, barY, barWidth, barHeight, fillColor);
  };

  if (obj.properties.displayValue == "none") {
    drawBar();
    drawSetpointMarker();
    return true;
  }


  String displayText;
  if (obj.properties.displayValue == "percentage") {
    displayText = String((int)(fillPercent + 0.5f)) + "%";
  } else if (obj.properties.displayValue == "value") {
    displayText = valueStr;
  }
  if (displayText.isEmpty()) return true;

  const BdfFont* font = fontFor(obj.properties.fontId);

  int16_t textWidth = measureTrueTextWidth(displayText, font);
  int16_t fontAscent = getFontAscentById(obj.properties.fontId);
  int16_t fontDescent = getFontDescentById(obj.properties.fontId);

  int16_t textX = obj.x + jsRound((obj.width - textWidth) / 2.0f);
  int16_t fontHeight = fontAscent + fontDescent;
  int16_t verticalCenterOffset = jsRound((obj.height - fontHeight) / 2.0f);
  int16_t textY = obj.y + verticalCenterOffset + fontAscent;

  // Pass 1: text in fillColor across the whole object. Clipped to the
  // object's own box, which is taller than the text - no ink overhang to
  // allow for here, unlike a label's font-sized box.
  canvas_->setClip(obj.x, obj.y, obj.width, obj.height);
  if (font != nullptr) font->drawText(*canvas_, displayText, textX, textY, fillColor);
  canvas_->clearClip();

  // Bar, painted solid over whatever text pass 1 drew there, then the marker
  // over the bar - the designer's order.
  drawBar();
  drawSetpointMarker();
  if (barWidth > 0 && barHeight > 0) {
    // Pass 2: the same text again, clipped to just the bar, in bgColor - so
    // the digits stay readable where the bar has painted over them.
    canvas_->setClip(barX, barY, barWidth, barHeight);
    if (font != nullptr) font->drawText(*canvas_, displayText, textX, textY, bgColor);
    canvas_->clearClip();
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
  // MqttEPaperDisplay2's identical fix for this exact confusion.
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
  String valueStr = projectLoader_.getTopicValue(obj.properties.topic);
  // No value yet: the track alone - no fill (not even what the calibration
  // makes of 0), no marker, no number (render-arc-level.ts, the same rule).
  String trimmedValue = valueStr;
  trimmedValue.trim();
  const bool noValue = trimmedValue.isEmpty();
  float value = noValue ? 0.0f : valueStr.toFloat();
  float fillPercent = noValue ? 0.0f : interpolateCalibration(value, obj.properties.calibrationPoints);
  if (fillPercent < 0.0f) fillPercent = 0.0f;
  if (fillPercent > 100.0f) fillPercent = 100.0f;

  // No setpoint topic, no marker - a tank level has nothing to aim at.
  //
  // Was ein Finger gerade angefordert hat, gewinnt aber auch hier, und braucht
  // dafuer kein Sollwert-Topic: ein Dimmer hat nur einen Wert am Broker
  // (Entscheidung 6c, dieselbe Regel wie im Balken oben).
  bool hasSetpoint = false;
  float setpointPercent = 0.0f;
  if (!noValue) {
    String setpointStr = obj.properties.setpointTopic.isEmpty()
                             ? String("")
                             : projectLoader_.getTopicValue(obj.properties.setpointTopic);
    if (!askedObjectId_.isEmpty() && obj.id == askedObjectId_ && !askedValue_.isEmpty()) {
      setpointStr = askedValue_;
    }
    if (!setpointStr.isEmpty()) {
      hasSetpoint = true;
      setpointPercent = interpolateCalibration(setpointStr.toFloat(), obj.properties.calibrationPoints);
      if (setpointPercent < 0.0f) setpointPercent = 0.0f;
      if (setpointPercent > 100.0f) setpointPercent = 100.0f;
    }
  }

  const int32_t size = min(obj.width, obj.height);
  if (size <= 0) return true;
  int32_t thickness = obj.properties.arcThickness;
  if (thickness < 1) thickness = 1;
  if (thickness > size / 2) thickness = size / 2;

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
  geom.track = ArcRaster::makeSector(start64, sweep64);
  geom.fill = ArcRaster::makeSector(fillStart64, filled64);
  if (hasSetpoint) {
    const int32_t at = ArcRaster::sweepForPercent(sweep64, setpointPercent);
    const int32_t centreAt = counterClockwise ? start64 + sweep64 - at : start64 + at;
    int32_t markerDeg = obj.properties.markerWidth;
    if (markerDeg < 1) markerDeg = 1;
    const int32_t width64 = markerDeg * ArcRaster::ANGLE_SCALE;
    geom.marker = ArcRaster::makeSector(centreAt - width64 / 2, width64);
  } else {
    geom.marker = ArcRaster::makeSector(0, 0);
  }

  bool backgroundTransparent = false;
  const uint16_t backgroundPacked =
      parseHexColor(obj.properties.backgroundColor.isEmpty() ? "transparent" : obj.properties.backgroundColor,
                    &backgroundTransparent);
  // Where the object declares no background of its own, the edges mix into
  // the screen's - known to the designer too, so both mix into the same
  // thing. Over a background image that is an approximation, but it is the
  // same approximation on both sides, so the comparison stays exact.
  const ArcRaster::Rgb565 mix = ArcRaster::toRgb565(backgroundTransparent ? mixInto : backgroundPacked);
  const ArcRaster::Rgb565 trackColour = ArcRaster::toRgb565(parseHexColor(obj.properties.trackColor));
  const ArcRaster::Rgb565 fillColour = ArcRaster::toRgb565(parseHexColor(obj.properties.fillColor));
  const ArcRaster::Rgb565 markerColour = ArcRaster::toRgb565(parseHexColor(obj.properties.markerColor));

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
  uint32_t bandsCycles = 0, drawCycles = 0, pixelsSeen = 0;
  // Counted unconditionally: the background runs below add whole spans at a
  // time, and branching on the profile for one addition per row is noise.
  uint32_t pixelsDrawn = 0;

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

    if (!backgroundTransparent) {
      // The object still owns its whole square, so what the ring cannot reach
      // is drawn as runs. The same pixels in the same colour as the per-pixel
      // version wrote, one contiguous write per gap instead of one per pixel.
      const uint32_t g0 = profiling ? esp_cpu_get_cycle_count() : 0;
      if (from > pxFrom) {
        canvas_->drawFastHLine(obj.x + pxFrom, obj.y + py, from - pxFrom, backgroundPacked);
        pixelsDrawn += from - pxFrom;
      }
      if (hasHole) {
        canvas_->drawFastHLine(obj.x + holeFrom, obj.y + py, holeTo + 1 - holeFrom, backgroundPacked);
        pixelsDrawn += holeTo + 1 - holeFrom;
      }
      if (to < pxTo) {
        canvas_->drawFastHLine(obj.x + to, obj.y + py, pxTo - to, backgroundPacked);
        pixelsDrawn += pxTo - to;
      }
      if (profiling) drawCycles += esp_cpu_get_cycle_count() - g0;
    }

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
      const int32_t covered = bands.fill + bands.track + bands.marker;

      if (covered == 0) {
        // Nothing of the ring here. With an opaque background the object
        // still owns its square; with a transparent one the pixel is left
        // alone - which is what "transparent" means everywhere in this
        // renderer: skip the draw, never mix.
        if (!backgroundTransparent) {
          uint32_t d0 = profiling ? esp_cpu_get_cycle_count() : 0;
          canvas_->drawPixel(obj.x + px, obj.y + py, backgroundPacked);
          if (profiling) {
            drawCycles += esp_cpu_get_cycle_count() - d0;
            pixelsDrawn++;
          }
        }
        continue;
      }

      const uint16_t mixed = ArcRaster::blendBands(fillColour, bands.fill, trackColour, bands.track, markerColour,
                                                   bands.marker, mix, (uint8_t)(ArcRaster::COVERAGE_MAX - covered));
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
    RenderProfile::note("  arc: " + String(pixelsSeen) + " px seen, " + String(pixelsDrawn) + " drawn");
    ArcRaster::probeEnabled() = false;
    const ArcRaster::Probe& pr = ArcRaster::probe();
    RenderProfile::add("  arc: early x" + String(pr.earlyCalls), pr.earlyCycles / 240);
    RenderProfile::add("  arc: sampled x" + String(pr.sampledCalls), pr.sampledCycles / 240);
    RenderProfile::add("  arc: uniform x" + String(pr.uniformCalls), pr.uniformCycles / 240);
  }

  if (obj.properties.displayValue == "none" || noValue) return true;

  String displayText;
  if (obj.properties.displayValue == "percentage") {
    displayText = String((int)(fillPercent + 0.5f)) + "%";
  } else if (obj.properties.displayValue == "value") {
    displayText = valueStr;
  }
  if (displayText.isEmpty()) return true;

  // Centred exactly as renderLevelIndicator centres its own value, so the
  // two siblings cannot drift apart on the one thing they both do.
  const BdfFont* font = fontFor(obj.properties.fontId);
  int16_t textWidth = measureTrueTextWidth(displayText, font);
  int16_t fontAscent = getFontAscentById(obj.properties.fontId);
  int16_t fontDescent = getFontDescentById(obj.properties.fontId);
  int16_t textX = obj.x + jsRound((obj.width - textWidth) / 2.0f);
  int16_t fontHeight = fontAscent + fontDescent;
  int16_t textY = obj.y + jsRound((obj.height - fontHeight) / 2.0f) + fontAscent;

  bool textTransparent = false;
  uint16_t textColor = parseHexColor(obj.properties.textColor.isEmpty() ? "#ffffff" : obj.properties.textColor,
                                     &textTransparent);
  if (!textTransparent && font != nullptr) {
    canvas_->setClip(obj.x, obj.y, obj.width, obj.height);
    font->drawText(*canvas_, displayText, textX, textY, textColor);
    canvas_->clearClip();
  }

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
void ColorScreenRenderer::drawSwitchBar(int x, int y, int width, uint16_t color, bool hollow) {
  int barWidth = width - 2 * kSwitchBarSideInset;
  if (barWidth < 4) barWidth = 4;
  int barX = x + kSwitchBarSideInset;
  int barY = y + kSwitchBarTopInset;

  if (hollow) {
    canvas_->drawRoundRect(barX, barY, barWidth, kSwitchBarHeight, kSwitchBarRadius, color);
  } else {
    canvas_->fillRoundRect(barX, barY, barWidth, kSwitchBarHeight, kSwitchBarRadius, color);
  }
}

// One state's icon + label inside the content band. Shared by both modes so
// a state cannot look different depending on which mode drew it - the same
// reason render-switch.ts factors this out.
void ColorScreenRenderer::drawSwitchStateContent(const SwitchState& state, const String& iconPath,
                                                 float x, float width, int bandTop, int bandHeight,
                                                 uint16_t textColor, const BdfFont* font,
                                                 int16_t fontAscent, int16_t fontDescent) {
  float centerX = x + width / 2.0f;
  bool hasIcon = !iconPath.isEmpty();

  int iconBottom = bandTop;
  if (hasIcon) {
    // Same expression, same rounding, as render-switch.ts and
    // lib/asset-export.ts's exportSwitchStateIcon (which is what baked this
    // bitmap at this exact size). 0.62 of a 32px band is 19.84 - truncating
    // where the others round would be a one-pixel offset on every icon.
    int iconSize = jsRound(fminf((float)(width - 8), bandHeight * 0.62f));
    if (iconSize < 1) iconSize = 1;
    int iconX = jsRound(centerX - iconSize / 2.0f);
    int iconY = bandTop + 2;
    iconBottom = iconY + iconSize + 2;
    ColorAssetLoader::drawBMPToCanvas(canvas_, iconPath, iconX, iconY);
  }

  if (state.label.isEmpty() || font == nullptr) return;

  int16_t fontHeight = fontAscent + fontDescent;
  int16_t textWidth = measureTrueTextWidth(state.label, font);
  // Float throughout, and rounded exactly once at the end - the same shape as
  // render-switch.ts's Math.round(centerX - min(textWidth, maxWidth) / 2).
  //
  // Rounding the half-difference first and adding a fractional x afterwards
  // is not the same sum: the assignment then truncates what the rounding
  // already moved, and the label lands a pixel left of the designer's on any
  // segment whose boundary is not a whole pixel. That was the last 170 pixels
  // of the switch's disagreement after the boundaries themselves were fixed
  // (2026-09-10).
  float maxWidth = width - 4.0f;
  float effectiveTextWidth = (float)textWidth;
  if (maxWidth > 0.0f && effectiveTextWidth > maxWidth) effectiveTextWidth = maxWidth;
  int16_t textX = jsRound(x + (width - effectiveTextWidth) / 2.0f);
  int16_t textY;
  if (hasIcon) {
    // Below the icon, top-anchored - matches render-switch.ts's anchorTop
    // path.
    textY = iconBottom + fontAscent;
  } else {
    // Centered in the content band, not in the whole object: the band
    // starts below the bar's reserved strip, so a label cannot land on top
    // of a marker.
    textY = bandTop + jsRound((bandHeight - fontHeight) / 2.0f) + fontAscent;
  }
  font->drawText(*canvas_, state.label, textX, textY, textColor);
}

bool ColorScreenRenderer::renderSwitch(const ScreenObject& obj, int pendingStateIndex, int pressedStateIndex) {
  bool bgTransparent = false;
  uint16_t bgColor = parseHexColor(obj.properties.backgroundColor, &bgTransparent);
  bool borderTransparent = false;
  uint16_t borderColor = parseHexColor(obj.properties.borderColor, &borderTransparent);
  uint16_t markerColor = parseHexColor(obj.properties.activeBackgroundColor);
  uint16_t textColor = parseHexColor(obj.properties.textColor);

  // Draw order, and why it is this one:
  //
  //   1 background, once over the whole object
  //   2 outer border
  //   3 dividers
  //   4 marker bars
  //   5 icons and labels
  //
  // Until 2026-08-25 the border was drawn first and then overpainted by the
  // per-segment fills - every one of its pixels; only the dividers survived,
  // because those came after their own segment's fill. Nobody noticed while
  // a large coloured block made the control obvious anyway. With the fill
  // gone the border is the only thing left that says where the object is.
  //
  // Filling once instead of per segment is what makes that free: no segment
  // has a fill of its own any more, so there is nothing left to overpaint
  // with. Content still comes last, matching ScreenRenderer's own order and
  // the reason recorded on the designer side on 2026-07-20 - a border drawn
  // after text erases glyph ink wherever a descender reaches the box edge.
  // Rounded body, drawn the way renderBox draws one: the border is not a
  // stroke but a slightly larger filled shape with the background filled on
  // top of it. That is the only way to get a rounded border out of
  // Adafruit's integer primitive, and it is what the designer's
  // render-switch.ts does with the same port of the same algorithm - hard
  // pixels, no anti-aliasing. Deliberately so: an anti-aliased edge is the
  // one thing that provably cannot survive the trip into this 16-bit panel
  // unchanged (2026-08-25, the SoftwareButton's baked corners).
  //
  // cornerRadius defaults to 0, so every project built before this looks
  // exactly as it did.
  int cornerRadius = obj.properties.cornerRadius;
  if (cornerRadius < 0) cornerRadius = 0;

  if (cornerRadius > 0) {
    if (!borderTransparent) {
      canvas_->fillRoundRect(obj.x, obj.y, obj.width, obj.height, cornerRadius, borderColor);
      if (!bgTransparent && obj.width > 2 && obj.height > 2) {
        int innerRadius = cornerRadius - 1;
        if (innerRadius < 0) innerRadius = 0;
        canvas_->fillRoundRect(obj.x + 1, obj.y + 1, obj.width - 2, obj.height - 2, innerRadius, bgColor);
      }
    } else if (!bgTransparent) {
      canvas_->fillRoundRect(obj.x, obj.y, obj.width, obj.height, cornerRadius, bgColor);
    }
  } else {
    if (!bgTransparent) {
      canvas_->fillRect(obj.x, obj.y, obj.width, obj.height, bgColor);
    }
    if (!borderTransparent) {
      canvas_->drawRect(obj.x, obj.y, obj.width, obj.height, borderColor);
    }
  }

  size_t stateCount = obj.properties.states.size();
  const BdfFont* font = fontFor(obj.properties.fontId);
  int16_t fontAscent = getFontAscentById(obj.properties.fontId);
  int16_t fontDescent = getFontDescentById(obj.properties.fontId);

  if (stateCount == 0) {
    // Mirrors render-switch.ts's own "No states defined" placeholder - an
    // empty/misconfigured Switch is still visible instead of a blank box.
    // The DDF's own first font, not a hardcoded u8g2 array - this device
    // may not publish helvR08 at all, and a placeholder is no reason to
    // reach for a font the DDF does not declare.
    const BdfFont* placeholderFont = fontStore_.get(fontStore_.defaultFontId());
    String placeholder = "No states defined";
    int16_t textWidth = measureTrueTextWidth(placeholder, placeholderFont);
    int16_t textX = obj.x + jsRound((obj.width - textWidth) / 2.0f);
    canvas_->setClip(obj.x, obj.y, obj.width, obj.height);
    // Mid gray; empty-state only, with no designer-side default to match.
    if (placeholderFont != nullptr) {
      placeholderFont->drawText(*canvas_, placeholder, textX, obj.y + obj.height / 2 + 4, 0x8410);
    }
    canvas_->clearClip();
    return true;
  }

  int activeIndex = getActiveSwitchStateIndex(obj);
  int bandTop = obj.y + kSwitchBarBand;
  int bandHeight = obj.height - kSwitchBarBand;

  if (obj.properties.mode == "single") {
    // One surface showing whichever state is active; main.cpp's tap handler
    // advances to the next one.
    bool unconfirmed = (pendingStateIndex >= 0 || pressedStateIndex >= 0);

    if (activeIndex < 0) {
      // Nothing matched, so there is no state - and every label belongs to
      // a state, so showing one would be claiming a state nobody reported.
      // "?" alone, which after a boot is what every tile shows until its
      // retained value arrives. A tap still gets its hollow bar: the write
      // went out even though the current value is unknown.
      if (unconfirmed) drawSwitchBar(obj.x, obj.y, obj.width, markerColor, true);
      if (font != nullptr) {
        int16_t textWidth = measureTrueTextWidth("?", font);
        int16_t textX = obj.x + jsRound((obj.width - textWidth) / 2.0f);
        int16_t textY = bandTop + jsRound((bandHeight - (fontAscent + fontDescent)) / 2.0f) + fontAscent;
        canvas_->setClip(obj.x, obj.y, obj.width, obj.height);
        font->drawText(*canvas_, "?", textX, textY, textColor);
        canvas_->clearClip();
      }
      return true;
    }

    const SwitchState& state = obj.properties.states[activeIndex];
    if (unconfirmed) {
      drawSwitchBar(obj.x, obj.y, obj.width, markerColor, true);
    } else if (state.showMarker) {
      drawSwitchBar(obj.x, obj.y, obj.width, markerColor, false);
    }

    canvas_->setClip(obj.x, obj.y, obj.width, obj.height);
    // No iconPathActive here: a state is only ever drawn in this mode while
    // it is the active one, so its own icon already is its active picture.
    // Reaching for the active variant would leave iconPath unreachable.
    drawSwitchStateContent(state, state.iconPath, obj.x, obj.width, bandTop, bandHeight,
                           textColor, font, fontAscent, fontDescent);
    canvas_->clearClip();
    return true;
  }

  // Float division, exactly as render-switch.ts does it, with rounding only
  // where the designer rounds.
  //
  // This used to be integer division, with the last segment absorbing the
  // remainder so the segments summed to obj.width - and a comment calling the
  // difference from the designer's float division "close enough to never
  // matter on a display this size". It matters whenever obj.width does not
  // divide by the state count: at 555px across two states the boundary falls
  // at 400.5, integer division put it at 400 and the designer's Math.round
  // put it at 401, so every divider and every marker bar sat one pixel apart.
  // Found 2026-09-10 by the generated type-coverage run, whose switch is a
  // width the hand-written fixtures never used (theirs is 280 across two
  // states, which divides exactly and hid this for as long as it existed).
  const float segmentWidth = (float)obj.width / (float)stateCount;
  // jsRound, not lroundf: the designer rounds with Math.round, which takes
  // .5 upward rather than away from zero. Identical for positive values, and
  // this is the file where "identical for the cases we tried" has already
  // cost one bug.
  auto boundary = [&](size_t index) {
    return jsRound((float)obj.x + (float)index * segmentWidth);
  };

  for (size_t i = 0; i < stateCount; i++) {
    int segX = boundary(i);
    // Two different widths, because the designer uses two. Content is clipped
    // to the gap between this boundary and the next, which tiles obj.width
    // exactly with no seam; the marker bar is sized from the segment width
    // itself (Math.round(width) in drawBar), which is the same number for
    // every segment even when the boundaries are not evenly spaced.
    int segW = boundary(i + 1) - segX;
    int barW = jsRound(segmentWidth);

    bool isPending = (int)i == pendingStateIndex;
    bool isPressed = (int)i == pressedStateIndex && !isPending;
    bool isActive = (int)i == activeIndex;

    // Divider between segments (not before the first one - the outer
    // border already covers that edge), same skip-if-transparent as the
    // outer border above.
    if (i > 0 && !borderTransparent) {
      canvas_->drawLine(segX, obj.y, segX, obj.y + obj.height - 1, borderColor);
    }

    // showMarker is a "single" mode concept: here the bar's whole job is to
    // say which of the n segments is active, which is not a question the
    // author gets to answer per state. An unconfirmed segment draws hollow
    // and the confirmed one stays solid, so during a write you see both -
    // "it is this, you asked for that" - and nothing has to lie.
    if (isPending || isPressed) {
      drawSwitchBar(segX, obj.y, barW, markerColor, true);
    } else if (isActive) {
      drawSwitchBar(segX, obj.y, barW, markerColor, false);
    }

    const SwitchState& state = obj.properties.states[i];
    // A genuinely different picture while this segment is active, falling
    // back to the normal one when the state declares none - which is now
    // the common case, since the exporter only ships a second file when the
    // author really picked a second icon (2026-08-25). Older exports, which
    // always carried one, keep working through the same branch.
    const String& iconPath = (isActive && !state.iconPathActive.isEmpty()) ? state.iconPathActive : state.iconPath;

    // Clips to this segment's own rect, not the whole control - matches
    // the designer's per-segment clip fix (2026-08-13): an oversized font
    // gets cropped at the segment boundary instead of bleeding into a
    // neighbor. Deliberately no ink-overhang allowance here: the clip is
    // the segment, which is taller than the text, so nothing is being
    // shaved off in the first place.
    canvas_->setClip(segX, obj.y, segW, obj.height);
    // Floats, so the centre matches. The designer centres on the unrounded
    // segment (x + width/2), and handing it a rounded pair here would shift
    // an icon or a label by a pixel on any boundary that is not whole.
    drawSwitchStateContent(state, iconPath, (float)obj.x + (float)i * segmentWidth, segmentWidth, bandTop, bandHeight,
                           textColor, font, fontAscent, fontDescent);
    canvas_->clearClip();
  }

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
  if (obj.type == "line") return renderLine(obj);
  if (obj.type == "label") return renderLabel(obj);
  if (obj.type == "MqttDataField" || obj.type == "field") return renderMQTTDataField(obj);
  if (obj.type == "level-indicator") return renderLevelIndicator(obj);
  if (obj.type == "arc-level") return renderArcLevel(obj, currentScreenBg_);
  if (obj.type == "MQTTIconField") return renderMQTTIconField(obj);
  if (obj.type == "icon") return renderIcon(obj);
  if (obj.type == "SoftwareButton") return renderSoftwareButton(obj, !pressedButtonId.isEmpty() && obj.id == pressedButtonId);
  if (obj.type == "Switch") {
    int pendingIndex = obj.id == pendingSwitchId ? pendingSwitchStateIndex : -1;
    int pressedIndex = obj.id == pressedSwitchId ? pressedSwitchStateIndex : -1;
    return renderSwitch(obj, pendingIndex, pressedIndex);
  }
  if (obj.type == "MqttDataLine") return renderMqttDataLine(obj);
  if (obj.type == "tab-control") {
    return renderTabControl(obj, pressedButtonId, pendingSwitchId, pendingSwitchStateIndex, pressedSwitchId, pressedSwitchStateIndex);
  }
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
    if (o->type == "tab-control") {
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
    if (o->type != "Switch" && o->type != "SoftwareButton" && !isSettableLevel(*o)) continue;

    const int16_t ox = dx + o->x;
    const int16_t oy = dy + o->y;
    if (x < ox || x >= ox + o->width) continue;
    if (y < oy || y >= oy + o->height) continue;
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
    if (o.type == "tab-control") {
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
  if (screenIndex < 0 || screenIndex >= (int)project.screens.size()) return nullptr;
  objX = 0;
  objY = 0;
  return findByIdWalk(project.screens[screenIndex].objects, id, 0, 0, objX, objY,
                      [this](const ScreenObject& tabControl) { return getActivePanel(tabControl); });
}

const ScreenObject* ColorScreenRenderer::hitTestTappable(int screenIndex, int16_t x, int16_t y,
                                                          int16_t& objX, int16_t& objY) const {
  if (!projectLoader_.isLoaded()) return nullptr;
  const ProjectConfig& project = projectLoader_.getProject();
  if (screenIndex < 0 || screenIndex >= (int)project.screens.size()) return nullptr;

  const ScreenObject* treffer = nullptr;
  objX = 0;
  objY = 0;
  hitTestWalk(project.screens[screenIndex].objects, 0, 0, x, y, treffer, objX, objY);
  return treffer;
}

bool ColorScreenRenderer::renderScreen(int screenIndex, const String& pressedButtonId, const String& pendingSwitchId,
                                        int pendingSwitchStateIndex, const String& pressedSwitchId,
                                        int pressedSwitchStateIndex) {
  if (!projectLoader_.isLoaded()) return false;

  const ProjectConfig& project = projectLoader_.getProject();
  if (screenIndex < 0 || screenIndex >= (int)project.screens.size()) return false;

  const Screen& screen = project.screens[screenIndex];

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

  std::vector<ScreenObject> sortedObjects = screen.objects;
  std::sort(sortedObjects.begin(), sortedObjects.end(), zOrderLess);

  for (const auto& obj : sortedObjects) {
    RenderProfile::Scope s(obj.type);
    renderObject(obj, pressedButtonId, pendingSwitchId, pendingSwitchStateIndex, pressedSwitchId, pressedSwitchStateIndex);
  }

  return true;
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
  if (screenIndex < 0 || screenIndex >= (int)project.screens.size()) return false;
  if (rw <= 0 || rh <= 0) return false;

  const Screen& screen = project.screens[screenIndex];

  bool bgTransparent = false;
  uint16_t screenBg = parseHexColor(screen.backgroundColor.isEmpty() ? "#ffffff" : screen.backgroundColor, &bgTransparent);
  currentScreenBg_ = bgTransparent ? 0xFFFF : screenBg;

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

bool ColorScreenRenderer::arcLevelDirtyRect(const ScreenObject& obj, const String& oldValue, const String& newValue,
                                            int16_t* outX, int16_t* outY, int16_t* outW, int16_t* outH) {
  if (obj.type != "arc-level") return false;
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
  const int32_t rOuter = size / 2;
  int32_t thickness = obj.properties.arcThickness;
  if (thickness < 1) thickness = 1;
  const int32_t rInner = rOuter - thickness > 0 ? rOuter - thickness : 0;

  int32_t minX = size, minY = size, maxX = -1, maxY = -1;
  const int STEPS = 24;
  for (int i = 0; i <= STEPS; i++) {
    const float pct = lo + (hi - lo) * (float)i / (float)STEPS;
    const int32_t at = ArcRaster::sweepForPercent(sweep64, pct);
    const int32_t angle64 = start64 + at;
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

  const int32_t PAD = 4;
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

