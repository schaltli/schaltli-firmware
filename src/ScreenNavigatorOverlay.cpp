#include "ScreenNavigatorOverlay.h"
#include "project/ColorAssetLoader.h"

namespace {
const unsigned long ENTER_MS = 250;
// 4s rather than the M5 Dial's 1.5s. There the overlay is an orientation
// aid that appears *while* you keep turning the encoder, so a short idle
// timeout is right. Here it is a menu you opened deliberately with a swipe
// and now have to aim at, so it has to outlast reading it and reaching for
// a tablet. Any tap or further swipe restarts the countdown.
const unsigned long HOLD_MS = 4000;
const unsigned long EXIT_MS = 200;

// 360x360 display, centre at (180,180). Every length below is the M5 Dial's
// value scaled by 1.5 to match - the layout was tuned by eye on a 240px
// panel, so keeping its proportions is truer to that work than keeping its
// pixel counts.
const int CENTER_X = 180;
const int CENTER_Y = 180;
const int CANVAS_SIZE = 360;
const int TABLET_RADIUS = 33;
const int ACTIVE_TABLET_RADIUS = 38;
const int TARGET_ARC_R = 132;
const int OUTER_ARC_R = 255;
const int ICON_SIZE = 56;  // must match the DDF's needsPageIconsInSize

// RGB565: #ff6600 (the M5 Dial adornment's own orange accent, see
// adornment.svg) for the active tablet, #333333 for inactive ones, white
// for the active ring and the icon stencil color.
const uint16_t COLOR_ACTIVE = 0xFB20;
const uint16_t COLOR_INACTIVE = 0x3186;
const uint16_t COLOR_WHITE = 0xFFFF;

// Per-tablet angular spacing along the arc portion of the path - fixed
// regardless of screen count (2026-08-11, replacing an earlier
// count-dependent span that compressed spacing as screens were added).
// Worst case center-to-center need = TABLET_RADIUS + ACTIVE_TABLET_RADIUS +
// a real gap (an inactive tablet sitting right next to the larger active
// one) = 33+38+6 = 77px. At TARGET_ARC_R (132), 34deg gives ~78px of
// spacing - clears that with the same small margin the 240px original had,
// which is why the angle survives the 1.5x scale unchanged.
const float STEP_DEG = 34.0f;
// The point along the arc where its tangent goes vertical (the rightmost/
// leftmost point of the circle, directly out from center) - beyond this,
// the path continues straight down instead of curving back in. See this
// class's header comment for why (an upside-down "U" with parallel legs,
// not a closing circle).
const float ARC_TO_STRAIGHT_DEG = 90.0f;
// How many tablets on each side of center to even consider drawing. Chosen
// generously (STEP_DEG=34 puts the arc/straight transition at ~2.6 steps,
// and by step 5 a resting-radius tablet has already scrolled off the
// canvas's bottom edge - see drawTablets()'s own off-canvas skip) - not a
// visible "window" size, just an upper bound so a very large project
// doesn't walk hundreds of screens every frame.
const int MAX_STEPS = 6;
}  // namespace

ScreenNavigatorOverlay::ScreenNavigatorOverlay(ClippedCanvas16* canvas, IProjectLoader& projectLoader)
  : canvas_(canvas), projectLoader_(projectLoader) {}

void ScreenNavigatorOverlay::beginOrExtend() {
  if (phase_ == Phase::Idle) {
    phase_ = Phase::Entering;
    phaseStartMs_ = millis();
  } else if (phase_ == Phase::Entering) {
    // Already mid fly-in - leave it alone and let the radius ramp finish
    // on its own schedule (update() flips Entering->Holding once ENTER_MS
    // has actually elapsed). Retriggering here on every single raw encoder
    // tick used to unconditionally jump straight to Holding, snapping the
    // radius to its resting value after only a few milliseconds - the
    // fly-in never got a chance to actually play while continuously
    // turning. That was invisible back when only the phase (not the
    // tablets' angular position too) changed per trigger, but very obvious
    // once continuous scrolling made the strip's position move in real
    // time alongside it - found live 2026-08-11 ("fly-in animation is
    // gone"). The caller already updated scrollPosition_ before calling
    // this, so the angular position keeps tracking the encoder in real
    // time regardless - only the radius ramp is left undisturbed here.
  } else {
    // Holding or Exiting - already fully visible at some point, or on its
    // way out. Jump/resume to Holding and restart the hold-timer countdown
    // - replaying the fly-in here would look like a stutter, not an
    // animation - see this class's own header comment.
    phase_ = Phase::Holding;
    phaseStartMs_ = millis();
  }
}

void ScreenNavigatorOverlay::trigger(int newScreenIndex) {
  scrollPosition_ = (float)newScreenIndex;
  beginOrExtend();
}

void ScreenNavigatorOverlay::triggerContinuous(float scrollPosition) {
  scrollPosition_ = scrollPosition;
  beginOrExtend();
}

bool ScreenNavigatorOverlay::update(ColorScreenRenderer* screenRenderer, int currentScreenIndex) {
  if (phase_ == Phase::Idle) return false;

  unsigned long elapsed = millis() - phaseStartMs_;
  float progress = 1.0f;

  if (phase_ == Phase::Entering) {
    progress = min(1.0f, (float)elapsed / (float)ENTER_MS);
    if (progress >= 1.0f) {
      phase_ = Phase::Holding;
      phaseStartMs_ = millis();
    }
  } else if (phase_ == Phase::Holding) {
    if (elapsed >= HOLD_MS) {
      phase_ = Phase::Exiting;
      phaseStartMs_ = millis();
      elapsed = 0;
    }
    progress = 1.0f;
  }
  if (phase_ == Phase::Exiting) {
    float exitProgress = min(1.0f, (float)elapsed / (float)EXIT_MS);
    progress = 1.0f - exitProgress;
    // Still draws this one last frame at progress==0 (tablets fully
    // retracted, so visually identical to no overlay at all) before
    // flipping to Idle - the following call returns false immediately,
    // no redundant redraw once nothing's changing anymore.
    if (exitProgress >= 1.0f) phase_ = Phase::Idle;
  }

  if (!screenRenderer) {
    phase_ = Phase::Idle;
    return false;
  }

  screenRenderer->renderScreen(currentScreenIndex);
  drawTablets(progress);
  return true;
}

void ScreenNavigatorOverlay::pathPosition(float offset, int r, int* outX, int* outY) const {
  float sign = offset < 0 ? -1.0f : 1.0f;
  float angleDeg = fabsf(offset) * STEP_DEG;

  int x, y;
  if (angleDeg <= ARC_TO_STRAIGHT_DEG) {
    // 0 degrees = straight up - screen coordinates put 0/90/180/270 at
    // right/down/left/up respectively, hence the -90 shift.
    float angleRad = (sign * angleDeg - 90.0f) * PI / 180.0f;
    x = CENTER_X + (int)(r * cosf(angleRad));
    y = CENTER_Y + (int)(r * sinf(angleRad));
  } else {
    // Past the tangent point: straight line down from there, at the same
    // per-degree spacing the arc used (arc length = r * angle-in-radians),
    // so a tablet's distance from its neighbors doesn't visibly change
    // where the path stops curving.
    float tangentRad = (sign * ARC_TO_STRAIGHT_DEG - 90.0f) * PI / 180.0f;
    int tx = CENTER_X + (int)(r * cosf(tangentRad));
    int ty = CENTER_Y + (int)(r * sinf(tangentRad));
    float extraDeg = angleDeg - ARC_TO_STRAIGHT_DEG;
    float extraDist = r * extraDeg * PI / 180.0f;
    x = tx;
    y = ty + (int)extraDist;
  }
  *outX = x;
  *outY = y;
}

void ScreenNavigatorOverlay::drawTablets(float progress) const {
  if (!projectLoader_.isLoaded()) return;
  const ProjectConfig& project = projectLoader_.getProject();
  int count = (int)project.screens.size();
  if (count == 0) return;

  int r = OUTER_ARC_R + (int)((TARGET_ARC_R - OUTER_ARC_R) * progress);

  // centerIdx: the screen index nearest the path's center-top point right
  // now; frac: how far scrollPosition_ actually is from that whole index
  // (-0.5..0.5). Every tablet's offset from center is k - frac, so at
  // frac==0 tablets sit exactly on their resting marks (matches the old
  // discrete behavior byte for byte), and as frac moves the whole strip
  // visibly slides - see this class's own header comment.
  int centerIdx = (int)roundf(scrollPosition_);
  float frac = scrollPosition_ - (float)centerIdx;

  // Small, fixed range around center rather than looping every screen -
  // see MAX_STEPS's own comment. No wrap-around (2026-08-11): a k that
  // lands outside the actual screen list is simply not drawn - the strip
  // ends at the first/last screen instead of cycling back, matching
  // main.cpp's clamped (not modulo) scrollPosition.
  for (int k = -MAX_STEPS; k <= MAX_STEPS; k++) {
    int idx = centerIdx + k;
    if (idx < 0 || idx >= count) continue;
    float offset = (float)k - frac;

    int cx, cy;
    pathPosition(offset, r, &cx, &cy);
    // Cheap off-canvas skip - avoids the (relatively expensive, LittleFS
    // file read) icon draw for tablets that have scrolled well past the
    // visible edge. Margin covers the tablet radius and icon size so a
    // partially-visible tablet right at the edge still gets drawn.
    if (cx < -ICON_SIZE || cx > CANVAS_SIZE + ICON_SIZE || cy < -ICON_SIZE || cy > CANVAS_SIZE + ICON_SIZE) {
      continue;
    }

    bool isActive = (k == 0);
    int tabletRadius = isActive ? ACTIVE_TABLET_RADIUS : TABLET_RADIUS;
    canvas_->fillCircle(cx, cy, tabletRadius, isActive ? COLOR_ACTIVE : COLOR_INACTIVE);
    if (isActive) {
      canvas_->drawCircle(cx, cy, tabletRadius, COLOR_WHITE);
    }

    const Screen& screen = project.screens[idx];
    if (!screen.pageIconPath.isEmpty()) {
      uint16_t tabletColor = isActive ? COLOR_ACTIVE : COLOR_INACTIVE;
      ColorAssetLoader::drawGrayscaleMaskToCanvas(canvas_, screen.pageIconPath, cx - ICON_SIZE / 2, cy - ICON_SIZE / 2, COLOR_WHITE, tabletColor);
    }
  }
}

// Mirrors drawTablets()'s layout exactly, at the resting radius
// (TARGET_ARC_R) rather than the current animation frame's - see the header
// comment for why a tablet still flying in should still be hittable where it
// is heading.
int ScreenNavigatorOverlay::screenAtPoint(int x, int y) const {
  if (phase_ == Phase::Idle || !projectLoader_.isLoaded()) return -1;
  const ProjectConfig& project = projectLoader_.getProject();
  int count = (int)project.screens.size();
  if (count == 0) return -1;

  int centerIdx = (int)roundf(scrollPosition_);
  float frac = scrollPosition_ - (float)centerIdx;

  // Nearest hit rather than first: the active tablet is drawn larger than
  // its neighbours, so their hit circles can overlap slightly. Picking the
  // closest centre makes a tap in the overlap land on the tablet whose
  // middle it is actually nearer, which is what the eye expects.
  int best = -1;
  long bestDistSq = 0;
  for (int k = -MAX_STEPS; k <= MAX_STEPS; k++) {
    int idx = centerIdx + k;
    if (idx < 0 || idx >= count) continue;

    int cx, cy;
    pathPosition((float)k - frac, TARGET_ARC_R, &cx, &cy);
    int radius = (idx == centerIdx) ? ACTIVE_TABLET_RADIUS : TABLET_RADIUS;
    long dx = x - cx;
    long dy = y - cy;
    long distSq = dx * dx + dy * dy;
    if (distSq > (long)radius * radius) continue;
    if (best == -1 || distSq < bestDistSq) {
      best = idx;
      bestDistSq = distSq;
    }
  }
  return best;
}
