#include "CanvasDisplayAdapter.h"

#include "extra/libs/qrcode/qrcodegen.h"
#include "BoardProfile.h"

// The panel is round: only the circle inscribed in the square framebuffer
// (360x360 on the knob, 390x390 on the T-Encoder Pro; BoardProfile.h)
// framebuffer can show anything, and the usable width shrinks the further a
// line sits from the vertical centre. Both the text size and the per-line
// safe width account for that - a fixed box would either waste most of the
// screen or push the ends of longer lines (an SSID, a URL) into the bezel.
static const int16_t CENTRE = BOARD_SCREEN_SIZE / 2;
static const int16_t RADIUS = CENTRE - 12; // a margin, so glyphs don't touch the edge

// Adafruit_GFX's built-in font is a fixed 6x8 cell scaled by setTextSize.
static int16_t charWidthFor(uint8_t size) { return (int16_t)(6 * size); }
static int16_t lineHeightFor(uint8_t size) { return (int16_t)(8 * size + 6); }

// Half the chord of the visible circle at a given row - i.e. how much width
// is actually on screen there.
static int16_t halfChordAt(int16_t y) {
  int32_t dy = y - CENTRE;
  int32_t inside = (int32_t)RADIUS * RADIUS - dy * dy;
  if (inside <= 0) return 0;
  return (int16_t)sqrt((double)inside);
}

void CanvasDisplayAdapter::clear() {
  ClippedCanvas16& canvas = presenter_.canvas();
  canvas.fillScreen(0x0000);
  presenter_.present();
}

void CanvasDisplayAdapter::showLines(std::initializer_list<String> lines) {
  ClippedCanvas16& canvas = presenter_.canvas();
  canvas.fillScreen(0x0000);
  canvas.setTextColor(0xFFFF);
  canvas.setTextWrap(false);

  int count = (int)lines.size();
  size_t longest = 0;
  for (const String& line : lines) longest = max(longest, (size_t)line.length());

  // Pick the largest size that still fits both ways, rather than a fixed
  // one: the setup screen is 10 lines and needs the small font, while a
  // two-line status message would be needlessly tiny at that size. Checked
  // against the widest line at the *outermost* row it will occupy, since
  // that is where the circle is narrowest.
  uint8_t size = 3;
  while (size > 1) {
    int16_t blockHeight = (int16_t)(count * lineHeightFor(size));
    int16_t topY = (int16_t)(CENTRE - blockHeight / 2);
    int16_t worstHalfChord = min(halfChordAt(topY), halfChordAt((int16_t)(topY + blockHeight)));
    bool fitsVertically = blockHeight <= 2 * RADIUS;
    bool fitsHorizontally = (int16_t)(longest * charWidthFor(size)) <= 2 * worstHalfChord;
    if (fitsVertically && fitsHorizontally) break;
    size--;
  }
  canvas.setTextSize(size);

  const int16_t lineHeight = lineHeightFor(size);
  const int16_t charWidth = charWidthFor(size);
  // Centred as a block, so a short message does not sit at the top of a
  // round screen with nothing under it.
  int16_t y = (int16_t)(CENTRE - (count * lineHeight) / 2);

  for (const String& line : lines) {
    int16_t textWidth = (int16_t)(line.length() * charWidth);
    int16_t half = halfChordAt((int16_t)(y + lineHeight / 2));
    int16_t safeLeft = (int16_t)(CENTRE - half);
    int16_t x = (int16_t)(CENTRE - textWidth / 2);
    if (x < safeLeft) x = safeLeft;
    // Clip rather than wrap: a line that still does not fit gets cut at the
    // circle instead of spilling into the bezel where nothing is visible.
    canvas.setClip(safeLeft, y, (int16_t)(2 * half), lineHeight);
    canvas.setCursor(x, y);
    canvas.print(line);
    canvas.clearClip();
    y += lineHeight;
  }

  presenter_.present();
  // present() only marks the area dirty - pump LVGL so the message is
  // actually on screen when this returns, since callers show status right
  // before doing something blocking.
  lv_timer_handler();
}

// Caption, QR code, then status lines - the AP setup screen.
//
// The code is drawn straight into the canvas rather than through LVGL's
// lv_qrcode widget: everything else on this screen is canvas drawing, and
// mixing a widget in would mean two things owning the same pixels during
// setup, when nothing else is running to arbitrate between them. The
// generator underneath is the same one either way.
//
// Sized to the round panel. A QR code is square and this screen is a
// circle, so the code goes in the middle where the chord is widest and the
// text takes the narrower rows above and below.
void CanvasDisplayAdapter::showQrScreen(const String& qrData, const String& caption,
                                        std::initializer_list<String> lines) {
  (void)caption;  // the SSID/PW lines below say what the code is for
  ClippedCanvas16& canvas = presenter_.canvas();
  canvas.fillScreen(0x0000);
  canvas.setTextColor(0xFFFF);
  canvas.setTextWrap(false);

  static uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(6)];
  static uint8_t scratch[qrcodegen_BUFFER_LEN_FOR_VERSION(6)];
  const bool ok = qrcodegen_encodeText(qrData.c_str(), scratch, qr, qrcodegen_Ecc_MEDIUM,
                                       qrcodegen_VERSION_MIN, 6, qrcodegen_Mask_AUTO, true);
  if (!ok) {
    showLines(lines);
    return;
  }

  // The code sits at the TOP, not the middle. A round screen is widest
  // across its centre, and that width belongs to the longest line of text -
  // an SSID like "waveshare-knob-1v8-setup" needs 288px at a size anyone
  // can read, which only exists near the middle. With the square in the
  // centre instead, the text had to shrink until it was reported
  // unreadable, and before that the SSID line was silently dropped
  // altogether for not fitting.
  const int16_t quietZone = 2;
  const int16_t qrBudget = BOARD_SETUP_QR_PX;  // the board's: what scans reliably on it
  const int qrModules = qrcodegen_getSize(qr);
  int16_t scale = (int16_t)(qrBudget / (qrModules + 2 * quietZone));
  if (scale < 1) scale = 1;
  const int16_t qrPixels = (int16_t)((qrModules + 2 * quietZone) * scale);
  const int16_t qrX = (int16_t)(CENTRE - qrPixels / 2);
  const int16_t qrY = 28;

  canvas.fillRect(qrX, qrY, qrPixels, qrPixels, 0xFFFF);
  for (int my = 0; my < qrModules; my++) {
    for (int mx = 0; mx < qrModules; mx++) {
      if (!qrcodegen_getModule(qr, mx, my)) continue;
      canvas.fillRect((int16_t)(qrX + (quietZone + mx) * scale),
                      (int16_t)(qrY + (quietZone + my) * scale), scale, scale, 0x0000);
    }
  }

  // Largest size at which *every* line fits, rather than a fixed one that
  // silently drops the ones that do not. A line vanishing because it was
  // too wide is the worst failure here: the screen looks fine and is
  // missing the password.
  const int16_t textTop = (int16_t)(qrY + qrPixels + 10);
  qrScreenTextTop_ = textTop;
  uint8_t size = 3;
  while (size > 1) {
    const int16_t lh = lineHeightFor(size);
    const int16_t cw = charWidthFor(size);
    int16_t y = textTop;
    bool all = true;
    for (const String& line : lines) {
      if (line.length() > 0 &&
          (int16_t)(line.length() * cw) > 2 * halfChordAt((int16_t)(y + lh / 2))) {
        all = false;
        break;
      }
      y = (int16_t)(y + lh);
    }
    if (all && y <= CENTRE + RADIUS) break;
    size--;
  }
  canvas.setTextSize(size);

  const int16_t lineHeight = lineHeightFor(size);
  const int16_t charWidth = charWidthFor(size);
  int16_t y = textTop;
  for (const String& line : lines) {
    if (line.length() > 0) {
      canvas.setCursor((int16_t)(CENTRE - (int16_t)line.length() * charWidth / 2), y);
      canvas.print(line);
    }
    y = (int16_t)(y + lineHeight);
  }

  presenter_.present();
}
