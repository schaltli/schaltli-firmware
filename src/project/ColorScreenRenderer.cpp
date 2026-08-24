#include "ColorScreenRenderer.h"
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
  uint16_t color = parseHexColor(obj.properties.color);
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
  uint16_t color = parseHexColor(obj.properties.color);

  std::vector<Point> points = obj.properties.points;
  if (points.size() < 2) {
    points.clear();
    points.push_back({obj.x, obj.y});
    points.push_back({obj.x + obj.width, obj.y + obj.height});
  }

  String rawValue = projectLoader_.getTopicValue(obj.properties.topic);
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

  String displayText = obj.properties.prefix + formattedValue + obj.properties.postfix;
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

  if (obj.properties.displayValue == "none") return true;

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

  // Bar, painted solid over whatever text pass 1 drew there.
  if (barWidth > 0 && barHeight > 0) {
    canvas_->fillRect(barX, barY, barWidth, barHeight, fillColor);

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
  float value = valueStr.isEmpty() ? 0.0f : valueStr.toFloat();
  float fillPercent = interpolateCalibration(value, obj.properties.calibrationPoints);
  if (fillPercent < 0.0f) fillPercent = 0.0f;
  if (fillPercent > 100.0f) fillPercent = 100.0f;

  // No setpoint topic, no marker - a tank level has nothing to aim at.
  bool hasSetpoint = false;
  float setpointPercent = 0.0f;
  if (!obj.properties.setpointTopic.isEmpty()) {
    String setpointStr = projectLoader_.getTopicValue(obj.properties.setpointTopic);
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

  for (int32_t py = pyFrom; py < pyTo; py++) {
    for (int32_t px = pxFrom; px < pxTo; px++) {
      const ArcRaster::PixelBands bands = ArcRaster::pixelBands(geom, px, py);
      const int32_t covered = bands.fill + bands.track + bands.marker;

      if (covered == 0) {
        // Nothing of the ring here. With an opaque background the object
        // still owns its square; with a transparent one the pixel is left
        // alone - which is what "transparent" means everywhere in this
        // renderer: skip the draw, never mix.
        if (!backgroundTransparent) {
          canvas_->drawPixel(obj.x + px, obj.y + py, backgroundPacked);
        }
        continue;
      }

      const uint16_t mixed = ArcRaster::blendBands(fillColour, bands.fill, trackColour, bands.track, markerColour,
                                                   bands.marker, mix, (uint8_t)(ArcRaster::COVERAGE_MAX - covered));
      canvas_->drawPixel(obj.x + px, obj.y + py, mixed);
    }
  }

  if (obj.properties.displayValue == "none") return true;

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
  const String& path = isPressed ? obj.pathActive : obj.pathNormal;
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
  for (size_t i = 0; i < obj.properties.states.size(); i++) {
    String readValue = obj.properties.states[i].readValue;
    readValue.trim();
    if (readValue == topicValue) return (int)i;
  }
  return -1;
}

// Segmented control, one segment per obj.properties.states entry - mirrors
// the designer's renderSwitch() (render-switch.ts) as closely as this
// firmware's text/color primitives allow: same draw order (outer border,
// then each segment's fill/divider/icon/label left-to-right), same active-
// segment resolution (getActiveSwitchStateIndex), same per-segment
// clipping so an oversized font can't bleed into a neighboring segment
// (2026-08-13 designer-side finding, ported here too). No tap/MQTT-write
// logic here - that's main.cpp's job (findSwitchSegmentAt + the pending/
// timeout state machine); this function only ever draws whatever
// pendingStateIndex/pressedStateIndex it's told, same "renderer just draws,
// main.cpp owns the state machine" split as renderSoftwareButton's isPressed.
bool ColorScreenRenderer::renderSwitch(const ScreenObject& obj, int pendingStateIndex, int pressedStateIndex) {
  bool borderTransparent = false;
  uint16_t borderColor = parseHexColor(obj.properties.borderColor, &borderTransparent);
  if (!borderTransparent) {
    canvas_->drawRect(obj.x, obj.y, obj.width, obj.height, borderColor);
  }

  size_t stateCount = obj.properties.states.size();
  if (stateCount == 0) {
    // Mirrors render-switch.ts's own "No states defined" placeholder - an
    // empty/misconfigured Switch is still visible (and selectable, once
    // touch hit-testing exists for it) instead of a blank box.
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

  bool bgTransparent = false;
  uint16_t bgColor = parseHexColor(obj.properties.backgroundColor, &bgTransparent);
  uint16_t activeBgColor = parseHexColor(obj.properties.activeBackgroundColor);
  uint16_t textColor = parseHexColor(obj.properties.textColor);
  uint16_t activeTextColor = parseHexColor(obj.properties.activeTextColor);

  const BdfFont* font = fontFor(obj.properties.fontId);
  int16_t fontAscent = getFontAscentById(obj.properties.fontId);
  int16_t fontDescent = getFontDescentById(obj.properties.fontId);
  int16_t fontHeight = fontAscent + fontDescent;

  int baseSegmentWidth = obj.width / (int)stateCount;

  for (size_t i = 0; i < stateCount; i++) {
    int segX = obj.x + (int)i * baseSegmentWidth;
    // Last segment absorbs the integer-division remainder so segments
    // always sum to exactly obj.width - the designer uses float division
    // (obj.width / states.length) instead, close enough a difference to
    // never matter on a display this size, but this keeps every pixel of
    // obj.width accounted for rather than losing up to (stateCount-1) px
    // off the right edge.
    int segW = (i + 1 == stateCount) ? (obj.x + obj.width - segX) : baseSegmentWidth;

    bool isPending = (int)i == pendingStateIndex;
    bool isPressed = (int)i == pressedStateIndex && !isPending;
    bool isActive = (int)i == activeIndex && !isPending;

    if (!bgTransparent) {
      canvas_->fillRect(segX, obj.y, segW, obj.height, isActive ? activeBgColor : bgColor);
    }

    // Pending: a thicker (3px) border in the active color instead of a
    // solid fill - visibly distinct from both "confirmed active" (solid
    // fill) and "plain inactive" (thin border only) without needing an
    // animation loop this single render call has no timer to drive.
    // Mirrors the "loading indicator" decision from this session's Switch
    // design discussion (2026-08-12) - main.cpp owns the actual 3s
    // pending/timeout state machine that decides pendingStateIndex; this
    // only draws whatever it's told.
    if (isPending) {
      for (int t = 0; t < 3; t++) {
        int ringW = segW - 2 * t;
        int ringH = obj.height - 2 * t;
        if (ringW <= 0 || ringH <= 0) break;
        canvas_->drawRect(segX + t, obj.y + t, ringW, ringH, activeBgColor);
      }
    }

    // Pressed: a single thin (1px) border in the active color while the
    // finger is still down, before release - immediate touch feedback,
    // deliberately thinner than pending's 3px ring so the two remain
    // visually distinct even though they're the same color (2026-08-14
    // addition, follow-up to this session's original pending-indicator
    // discussion once "does a press register at all" turned out to have no
    // feedback whatsoever until release).
    if (isPressed) {
      canvas_->drawRect(segX, obj.y, segW, obj.height, activeBgColor);
    }

    // Divider between segments (not before the first one - the outer
    // border already covers that edge), same skip-if-transparent as the
    // outer border above.
    if (i > 0 && !borderTransparent) {
      canvas_->drawLine(segX, obj.y, segX, obj.y + obj.height - 1, borderColor);
    }

    const SwitchState& state = obj.properties.states[i];

    // Icon - same iconSize/iconX/iconY formula as render-switch.ts and
    // lib/asset-export.ts's exportSwitchStateIcon() (which is what actually
    // baked this bitmap at this exact size/position), so the designer
    // preview, the export bake, and this draw all agree. Picks
    // iconPathActive over iconPath while the segment is showing its active
    // fill - the two are baked against different backgrounds
    // (backgroundColor vs. activeBackgroundColor) specifically so the
    // icon's own backdrop always matches what's actually behind it
    // (2026-08-14 fix: a single bake looked wrong the moment its segment
    // went active). Falls back to iconPath if no active variant exists
    // (an older export, before this fix). No placeholder-on-missing-asset
    // here (unlike renderIcon/renderMQTTIconField) - an empty path just
    // means this state has no icon configured, not a load failure to flag.
    int16_t iconBottom = obj.y;
    const String& iconPath = (isActive && !state.iconPathActive.isEmpty()) ? state.iconPathActive : state.iconPath;
    bool hasIcon = !iconPath.isEmpty();
    if (hasIcon) {
      int iconSize = (int)fminf((float)(segW - 8), obj.height * 0.5f);
      int iconX = segX + segW / 2 - iconSize / 2;
      int iconY = obj.y + 4;
      iconBottom = iconY + iconSize + 2;
      ColorAssetLoader::drawBMPToCanvas(canvas_, iconPath, iconX, iconY);
    }

    if (state.label.isEmpty()) continue;

    int16_t textWidth = measureTrueTextWidth(state.label, font);
    int16_t maxWidth = segW - 4;
    if (maxWidth > 0 && textWidth > maxWidth) textWidth = maxWidth;
    int16_t textX = segX + jsRound((segW - textWidth) / 2.0f);
    int16_t textY;
    if (hasIcon) {
      // Below the icon, top-anchored (not vertically centered in the whole
      // segment) - matches render-switch.ts's anchorTop path.
      textY = iconBottom + fontAscent;
    } else {
      int16_t verticalCenterOffset = jsRound((obj.height - fontHeight) / 2.0f);
      textY = obj.y + verticalCenterOffset + fontAscent;
    }

    // Clips to this segment's own rect, not the whole control - matches
    // the designer's per-segment clip fix (2026-08-13): an oversized font
    // gets cropped at the segment boundary instead of bleeding into a
    // neighbor. Deliberately no ink-overhang allowance here: the clip is
    // the segment, which is taller than the text, so nothing is being
    // shaved off in the first place.
    canvas_->setClip(segX, obj.y, segW, obj.height);
    if (font != nullptr) {
      font->drawText(*canvas_, state.label, textX, textY, isActive ? activeTextColor : textColor);
    }
    canvas_->clearClip();
  }

  return true;
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
    [](const ScreenObject* a, const ScreenObject* b) {
      return a->zIndex < b->zIndex;
    });

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
  std::sort(sortedChildren.begin(), sortedChildren.end(),
    [](const ScreenObject& a, const ScreenObject& b) {
      return a.zIndex < b.zIndex;
    });

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

bool ColorScreenRenderer::renderScreen(int screenIndex, const String& pressedButtonId, const String& pendingSwitchId,
                                        int pendingSwitchStateIndex, const String& pressedSwitchId,
                                        int pressedSwitchStateIndex) {
  if (!projectLoader_.isLoaded()) return false;

  const ProjectConfig& project = projectLoader_.getProject();
  if (screenIndex < 0 || screenIndex >= (int)project.screens.size()) return false;

  const Screen& screen = project.screens[screenIndex];

  bool bgTransparent = false;
  uint16_t screenBg = parseHexColor(screen.backgroundColor.isEmpty() ? "#ffffff" : screen.backgroundColor, &bgTransparent);
  canvas_->fillScreen(bgTransparent ? 0xFFFF : screenBg);
  // Kept for the objects that need to know what they are drawn onto - an
  // arc-level mixes its anti-aliased edges into this when its own
  // background is transparent, which is the usual case for a ring.
  currentScreenBg_ = bgTransparent ? 0xFFFF : screenBg;

  std::vector<ScreenObject> sortedObjects = screen.objects;
  std::sort(sortedObjects.begin(), sortedObjects.end(),
            [](const ScreenObject& a, const ScreenObject& b) { return a.zIndex < b.zIndex; });

  for (const auto& obj : sortedObjects) {
    renderObject(obj, pressedButtonId, pendingSwitchId, pendingSwitchStateIndex, pressedSwitchId, pressedSwitchStateIndex);
  }

  return true;
}

// Ausschnittfassung. Teilt sich mit renderScreen() bewusst NICHT nur die
// Objektliste, sondern auch deren Sortierung und den Hintergrund - jede
// Abweichung waere ein Pixelunterschied, und genau darauf pruefen die
// HIL-Suiten.
bool ColorScreenRenderer::renderScreenRegion(int screenIndex, int16_t rx, int16_t ry, int16_t rw, int16_t rh) {
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
  std::sort(sortedObjects.begin(), sortedObjects.end(),
            [](const ScreenObject& a, const ScreenObject& b) { return a.zIndex < b.zIndex; });
  for (const auto& obj : sortedObjects) {
    renderObject(obj, "", "", -1, "", -1);
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

