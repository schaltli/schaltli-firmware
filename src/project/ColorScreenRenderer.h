#pragma once
#include <Arduino.h>
#include <Adafruit_GFX.h>
#include "BdfFont.h"
#include "DdfFontStore.h"
#include "../interfaces/IProjectLoader.h"
#include "ProjectTypes.h"
#include "../ClippedCanvas16.h"
#include "ColorAssetLoader.h"
#include "LevelShape.h"
#include "PillRaster.h"
#include "ArcRaster.h"
#include <functional>

// Color (RGB565) counterpart of schaltli-eink's ScreenRenderer -
// renders into a ClippedCanvas16 instead of a 1-bit ClippedCanvas1, real
// hex colors instead of black/white thresholding, otherwise mirrors that
// class's carefully HIL-tuned pixel logic (drawTextBox's background/
// border/text order and +1/center-align math, measureTrueTextWidth's
// dry-run measurement technique, u8g2 setFont-before-setFontMode
// ordering) as closely as possible - those fixes are color-depth-
// independent, so there's no reason to rediscover them here.
//
// Every object type in the M5 Dial DDF's supportedObjectTypes is now
// ported: box, line (now full parity with the e-paper reference - fillet,
// thick strokes, fixed arrowheads), label, MqttDataField, level-indicator,
// MQTTIconField, icon, SoftwareButton, tab-control, panel, MqttDataLine.
// icon and SoftwareButton have no e-paper equivalent to port from -
// schaltli-eink never implemented either (no touchscreen, and its own
// "icon" object type was never wired up despite existing in the schema) -
// so their rendering (draw the already-exported bitmap via ColorAssetLoader,
// always the "normal" state for SoftwareButton until real touch tracking
// exists) was designed fresh from the designer's own export shape
// (lib/project-zip.ts's path/pathNormal/pathActive fields), not ported from
// existing firmware logic.
//
// tab-control/panel/MqttDataLine (2026-08-14 port from schaltli-eink's
// ScreenRenderer) reuse that firmware's logic near-verbatim, swapping only
// parseColor()'s black/white result for parseHexColor()'s real RGB565 one -
// the geometry (fillet math, thick-line rasterization, arrowhead placement,
// panel-condition evaluation) is color-depth-independent. NOTE: touch
// hit-testing (main.cpp's findSoftwareButtonAt/findSwitchSegmentAt) is
// still top-level-only and does NOT recurse into a tab-control's active
// panel - a SoftwareButton or Switch nested inside a panel renders
// correctly but isn't tappable yet. Out of scope here (this port covers
// rendering only, per the request that introduced it); see those
// functions' own comments.
// One pill-shaped run and the colour it is painted in - the counterpart of
// PaintedPill in the designer's components/canvas/renderers/paint-pills.ts.
//
// It lives here rather than in PillRaster.h because the designer keeps colour
// out of lib/pill-raster.ts too: that file is geometry, and where the geometry
// is painted is the renderer's business.
//
// `paints == false` is a hole: the run claims its pixels - nothing behind it
// shows through - but paints nothing, which is how an outline is described
// (the outer pill in the outline's colour, the inside knocked out of it).
struct PaintedPill {
  PillRaster::Band band;
  uint16_t colour = 0;
  bool paints = true;
  // Painted in the gradient of the PillShading handed to paintPills rather
  // than in `colour` - a fill that runs from one colour to another along its
  // bar (LevelGlow.h).
  bool graded = false;
};

// A bar's fill as a gradient, and the glow around its filled part - what
// PaintedPill.colourAt and PillGlow are in the designer's paint-pills.ts, as
// numbers rather than callbacks. Positions are whole pixels along the track's
// axis, the glow's capsule in 1/8 pixel.
struct PillShading {
  ArcRaster::Rgb565 from;
  ArcRaster::Rgb565 to;
  bool vertical = false;
  bool fromEnd = false;
  int32_t trackStart = 0;
  int32_t trackLength = 1;
  // The glow: none where glowPx is 0.
  int32_t glowPx = 0;
  int32_t edge = 0;
  int32_t half = 0;
  int32_t cross = 0;
  int32_t a = 0;
  int32_t b = 0;
  // The object's own box, which the glow never leaves.
  int32_t boxX = 0;
  int32_t boxY = 0;
  int32_t boxW = 0;
  int32_t boxH = 0;
};

class ColorScreenRenderer {
public:
  ColorScreenRenderer(IProjectLoader& projectLoader, ClippedCanvas16* canvas);

  // Redirects rendering to another canvas of the same size. Used to draw the
  // incoming screen of a swipe transition into CanvasPresenter's second
  // buffer while the outgoing one is still on display. A setter rather than
  // a second renderer instance because this one caches decoded assets - two
  // instances would decode and hold every image twice.
  // Text goes through canvas_ like everything else now - BdfFont writes
  // pixels directly - so there is no second drawing target to keep in
  // step. The u8g2 wrapper held its own pointer, bound once in the
  // constructor, and repointing canvas_ alone sent every glyph to the old
  // canvas: a screen rendered into the transition buffer arrived complete
  // except for its text. That whole class of bug goes with the second
  // target.
  void setCanvas(ClippedCanvas16* canvas) { canvas_ = canvas; }

  // A popup (index as screenOrPopupAt(), ProjectTypes.h) drawn over what the
  // canvas holds: its background inside the fence, then its objects. The
  // screen underneath stays; dimOutsidePopupFence() sets it back first on an
  // RGB board (designer docs/device-contract.md 2.5).
  // borderPx: how wide the fence's edge is drawn in the popup's border colour
  // (0 for none) - the board's choice.
  bool renderPopup(int popupIndex, int borderPx = 2, const String& pressedButtonId = "",
                   const String& pendingSwitchId = "", int pendingSwitchStateIndex = -1,
                   const String& pressedSwitchId = "", int pressedSwitchStateIndex = -1);
  void drawPopupBorder(const Screen& popup, int borderPx);
  // The round close button on the fence's edge (PopupFence.h), drawn over a
  // popup's objects by renderPopup() and by a region render that reaches it.
  // Radius 0, the default, draws none: a board opts in.
  void setPopupCloseBadgeRadius(int radius) { popupCloseBadgeRadius_ = radius; }
  PopupCloseBadge popupCloseBadge() const;
  void drawPopupCloseBadge(const Screen& popup);
  // A project colour as the canvas takes it - for a board drawing a popup's
  // frame itself.
  uint16_t rgb565Of(const String& hex, bool* isTransparent = nullptr) const { return parseHexColor(hex, isTransparent); }
  void dimOutsidePopupFence();
  // Every pixel of the canvas at half: the scrim over the whole screen, for a
  // board whose popup grows from its button - the scrim reaches up to the
  // window at every size on the way, not only at the last.
  void dimCanvas();

  // What `{device:id}` shows in a text: the MQTT client id, which the
  // renderer has no way to know by itself. Until it is set, empty.
  void setDeviceId(const String& id) { deviceId_ = id; }

  // A text with its placeholders resolved - `{topic:…}` from the topic's
  // last value, `{device:model}` and `{device:id}`, the project's
  // separators (designer docs/2026-10-05-placeholder-devices.md). A text
  // without a `{` comes back as it is.
  String resolvePlaceholders(const String& text) const;

  // Whether a text object names `topic` (bare, no `#json.path`) in a
  // placeholder - what a partial redraw asks when that topic changes.
  static bool textNamesTopic(const ScreenObject& obj, const String& topic);

  // Parses every font the loaded project references, up front.
  //
  // Fonts load lazily on first use otherwise, and that first use is inside
  // a render - measured at 275ms for one .bdf, on top of the 177ms the
  // render itself takes. Paid at boot that is invisible; paid when a drag
  // starts pre-rendering a neighbouring screen it is a stall in the middle
  // of a gesture, which is the exact fault the neighbour pre-render exists
  // to remove.
  void preloadFonts();
  ClippedCanvas16* canvas() const { return canvas_; }

  // Renders every object on the given screen index into the canvas
  // (z-index order, matching sortChildrenByZIndex on the designer side).
  // Does NOT blit to the display - caller does that once after this
  // returns, same as the color-canvas spike (checkpoint 2a).
  // pressedButtonId: if non-empty and it matches a SoftwareButton's own id
  // on this screen, that one button draws pathActive instead of pathNormal
  // - see main.cpp's touch handling for why a full re-render (not a
  // partial single-object update) is used for this: cheap enough on a
  // color LCD with no e-paper ghosting concern to optimize around, same
  // reasoning as the existing MQTT-triggered redraw path.
  // pendingSwitchId/pendingSwitchStateIndex: while a Switch's tapped
  // segment is awaiting MQTT confirmation (main.cpp's 3s pending/timeout
  // state machine), that one segment draws the pending look instead of its
  // normal active/inactive one - same "pass the one runtime highlight down
  // through render" pattern as pressedButtonId, just keyed by (object id,
  // segment index) instead of just an id, since a Switch has more than one
  // sub-region that can independently need this.
  // pressedSwitchId/pressedSwitchStateIndex: while a Switch segment is
  // actively being touched (finger still down, before release) - draws a
  // blended background/text color (halfway between the segment's normal
  // and active colors) so a press is felt immediately, distinct from both
  // the plain idle look and the thicker-bordered pending look that
  // replaces it the moment the finger lifts (2026-08-14 addition, agreed
  // in this session's follow-up design discussion).
  // Zeichnet nur ein Rechteck des Bildschirms neu - Hintergrund und alle
  // Objekte, aber auf den Ausschnitt begrenzt. Innerhalb des Rechtecks ist
  // das Ergebnis bitgleich zu renderScreen(), weil dieselbe Reihenfolge auf
  // denselben Hintergrund gezeichnet wird; ausserhalb bleibt das vorige Bild
  // stehen. Die Pixel-Gleichheit mit dem Designer bleibt damit erhalten.
  // Die Rueckmeldeparameter sind dieselben wie bei renderScreen() und
  // werden genauso durchgereicht. Sie haben Vorgabewerte, weil dieser
  // Ausschnitt-Render bis 2026-09-10 gar keine kannte: er zeichnete jedes
  // Objekt ungedrueckt, was fuer den Bogen-Ausschnitt des Knobs richtig war
  // und einen gedrueckten Knopf unmoeglich machte. Ein Tastendruck ist genau
  // der Fall, fuer den sich ein Vollbild nicht lohnt.
  bool renderScreenRegion(int screenIndex, int16_t rx, int16_t ry, int16_t rw, int16_t rh,
                          const String& pressedButtonId = "", const String& pendingSwitchId = "",
                          int pendingSwitchStateIndex = -1, const String& pressedSwitchId = "",
                          int pressedSwitchStateIndex = -1);

  // Welches Rechteck sich aendert, wenn dieser Ring von oldValue auf newValue
  // geht - Bogenenden plus die Zahl in der Mitte. Liefert false, wenn sich
  // nichts aendert oder der Fall nicht sicher eingegrenzt werden kann; dann
  // muss der Aufrufer ein Vollbild zeichnen.
  // Das Rechteck der Zahl in der Mitte, getrennt vom Bogenabschnitt.
  bool arcLevelTextRect(const ScreenObject& obj, const String& oldValue, const String& newValue,
                        int16_t* outX, int16_t* outY, int16_t* outW, int16_t* outH);

  bool arcLevelDirtyRect(const ScreenObject& obj, const String& oldValue, const String& newValue,
                         int16_t* outX, int16_t* outY, int16_t* outW, int16_t* outH);
  // The middle of a ring where its numbers stand - both of them, the asked one
  // and the measured one under it - across the whole hole.
  bool arcLevelCentreRect(const ScreenObject& obj, int16_t* outX, int16_t* outY, int16_t* outW, int16_t* outH);
  // A bar or slider going from oldValue to newValue: the stretch of it between
  // the two positions - fill edge, handle or pointer, and the glow's end -
  // across the whole object, and the header or number column beside it.
  // Writes up to two rectangles, returns how many, or -1 where it cannot say.
  int barLevelDragRects(const ScreenObject& obj, const String& oldValue, const String& newValue, int16_t* rx,
                        int16_t* ry, int16_t* rw, int16_t* rh);

  bool renderScreen(int screenIndex, const String& pressedButtonId = "", const String& pendingSwitchId = "",
                     int pendingSwitchStateIndex = -1, const String& pressedSwitchId = "",
                     int pressedSwitchStateIndex = -1);

  // Was an dieser Stelle obenauf liegt und einen Tipp annehmen kann, oder
  // nullptr. `objX`/`objY` bekommen die ABSOLUTE linke obere Ecke des
  // Treffers - der Aufrufer braucht sie fuer die Segmentrechnung, und bei
  // einem Objekt in einem Panel ist sie nicht obj.x/obj.y.
  //
  // Hier statt in main.cpp, weil die Antwort an zwei Dingen haengt, die
  // bereits hier leben: welches Panel eines tab-control gerade sichtbar ist
  // (getActivePanel), und um wieviel dessen Kinder verschoben gezeichnet
  // werden. Eine zweite Kopie hiesse, dass Finger und Bild ueber
  // verschiedene Geometrien reden - dieselbe Begruendung, aus der
  // getActiveSwitchStateIndex() am 2026-08-25 oeffentlich wurde.
  const ScreenObject* hitTestTappable(int screenIndex, int16_t x, int16_t y,
                                      int16_t& objX, int16_t& objY) const;

  // Was der Renderer auf diesem Screen SIEHT, eine Zeile je Objekt.
  //
  // Entstanden 2026-09-10, weil ein MqttDataField auf dem Knob gar nichts
  // zeichnete - kein Text, kein Hintergrund - und von aussen nicht zu
  // unterscheiden war, ob es fehlt, ob seine Schrift fehlt oder ob sein Wert
  // leer ist. Genau diese drei Faelle sehen auf dem Glas identisch aus, und
  // eine Stunde Codelesen hat sie nicht getrennt.
  String describeScreen(int screenIndex);

private:
  // Laeuft in Zeichenreihenfolge; der LETZTE Treffer gewinnt, weil zuletzt
  // gezeichnet heisst obenauf. Ueber Ebenen hinweg ist das richtig, ein
  // Vergleich roher zIndex-Werte waere es nicht: der zIndex eines
  // Panel-Kindes gilt unter seinen Geschwistern, nicht gegen die oberste
  // Ebene.
  void hitTestWalk(const std::vector<ScreenObject>& objects, int16_t dx, int16_t dy,
                   int16_t x, int16_t y, const ScreenObject*& treffer,
                   int16_t& objX, int16_t& objY) const;

  IProjectLoader& projectLoader_;
  ClippedCanvas16* canvas_;
  String deviceId_;
  int popupCloseBadgeRadius_ = 0;
  DdfFontStore fontStore_;

  bool renderObject(const ScreenObject& obj, const String& pressedButtonId, const String& pendingSwitchId,
                     int pendingSwitchStateIndex, const String& pressedSwitchId, int pressedSwitchStateIndex);
  bool renderBox(const ScreenObject& obj);
  bool renderLine(const ScreenObject& obj);
  bool renderLabel(const ScreenObject& obj);
  bool renderMQTTDataField(const ScreenObject& obj);
  bool renderLevelIndicator(const ScreenObject& obj);
  // The object's font, as the number column's measure - ascent, descent and
  // the height of a capital.
  LevelFontMetrics levelMetrics(const ScreenObject& obj);

  // One run of text inside the level indicator's number column, clipped to it.
  // Mirrors drawLevelTextIn() in the designer's render-level-indicator.ts.
  void drawLevelText(const String& text, const LevelRect& clip, int x, int baseline, const String& fontId,
                     uint16_t color);
  // The smaller font a ring's measured value is written in, out of the
  // project's own list - a BDF font cannot be scaled, so "smaller" means
  // another font. Mirrors levelSubFont() in the designer's
  // render-level-indicator.ts.
  String levelSubFontId(const ScreenObject& obj);
  // The round counterpart of the bar. `mixInto` is what its anti-aliased
  // edges blend against where the object's own background is transparent -
  // a declared colour rather than whatever happens to be on the canvas,
  // because the designer cannot read its canvas back at an arbitrary zoom
  // and an edge that mixes into different things on the two sides is an
  // edge that fails the pixel comparison.
  bool renderArcLevel(const ScreenObject& obj, uint16_t mixInto);

  // Paints a set of pill-shaped runs, anti-aliased, in one pass - this repo's
  // copy of the designer's components/canvas/renderers/paint-pills.ts.
  //
  // The runs come in priority order - a handle before the track it lies on, a
  // hole before the ring it cuts - because every sub-sample is counted into
  // exactly one of them (PillRaster.h). That is the whole reason this exists
  // rather than a sequence of fillRoundRect calls: where two runs meet,
  // painting one over the other leaves a seam of the lower one's colour along
  // the join.
  //
  // Straight onto the canvas, not through a buffer of its own: there is no
  // zoom here, so there is no second softening to avoid - the designer draws
  // into an offscreen buffer only because its canvas is scaled.
  //
  // `mixInto` is what a soft edge blends against where nothing of the control
  // covers a pixel: a declared colour rather than whatever happens to be on
  // the canvas, exactly as renderArcLevel's own mixInto is, and for the same
  // reason - the designer cannot read its canvas back at an arbitrary zoom.
  //
  // Only ever called where PILL_ANTIALIASED is 1. Below 24 bit the callers
  // keep their whole-pixel path; see PillRaster.h for why that is a decision.
  void paintPills(const std::vector<PaintedPill>& painted, uint16_t mixInto,
                  const PillShading* shading = nullptr);

  // The background renderScreen() filled before drawing any object.
  uint16_t currentScreenBg_ = 0xFFFF;
  // The same colour as the author wrote it, which is what a derived colour has
  // to be mixed from: going through 565 first and back would land a step away
  // from the designer's own mix (src/project/MaterialColors.h).
  String currentScreenBgHex_ = "#ffffff";
  bool renderMQTTIconField(const ScreenObject& obj);
  bool renderIcon(const ScreenObject& obj);
  bool renderSoftwareButton(const ScreenObject& obj, bool isPressed);
  bool renderSwitch(const ScreenObject& obj, int pendingStateIndex, int pressedStateIndex);
  // Switch marker bar - solid for a confirmed marked state, hollow while a
  // write is unconfirmed. Geometry constants live next to the definition in
  // ColorScreenRenderer.cpp, mirroring render-switch.ts.
  bool renderMqttDataLine(const ScreenObject& obj);

  // Recurses into the active panel's children (offset-copies with
  // obj.x/obj.y added, since child coordinates are relative to the
  // tab-control's own origin) via renderObject() - draws nothing itself if
  // no panel matches. Mirrors schaltli-eink's ScreenRenderer::
  // renderTabControl() and components/canvas/renderers'
  // renderScreenObjects()'s "tab-control" case on the designer side. Takes
  // the same touch-state params renderObject() does, so a SoftwareButton/
  // Switch nested inside the active panel still renders its pressed/pending
  // look correctly even though it can't be hit-tested yet (see this file's
  // header comment).
  bool renderTabControl(const ScreenObject& obj, const String& pressedButtonId, const String& pendingSwitchId,
                         int pendingSwitchStateIndex, const String& pressedSwitchId, int pressedSwitchStateIndex);

  // Walks tabControl.children (each expected type "panel") in zIndex order,
  // returns a pointer to the first whose condition matches the
  // tab-control's own topic value, nullptr if none match. Pointer is valid
  // as long as tabControl's own children vector isn't mutated/destroyed -
  // callers only ever use it within the same renderObject() call. Mirrors
  // schaltli-eink's ScreenRenderer::getActivePanel() exactly.
  const ScreenObject* getActivePanel(const ScreenObject& tabControl) const;

  bool evaluateCondition(float value, const String& op, float threshold) const;

  // Generalization of evaluateCondition() for panel matching: "==" / "!="
  // compare as trimmed Strings (an enum mode like "TEMP" isn't numeric, so
  // the float-only evaluateCondition() can't distinguish "TEMP" from
  // "POWER" - both toFloat() to 0.0), the four numeric operators delegate
  // to the existing evaluateCondition() via .toFloat() on both sides.
  // Mirrors schaltli-eink's ScreenRenderer::evaluateVisibilityCondition()
  // and lib/render-screen.ts's evaluateCondition() on the designer side
  // exactly, the 0.0-not-NaN fallback included.
  bool evaluateVisibilityCondition(const String& actualValue, const String& op, const String& comparisonValue) const;

  String getIconPathForValue(const ScreenObject& obj, const String& valueStr) const;
  // Index into obj.properties.states whose readValue (trimmed) equals the
  // Switch's own topic's current value (also trimmed) - mirrors the
  // designer's getActiveSwitchStateIndex() (render-switch.ts) exactly:
  // plain trimmed String equality, not evaluateCondition()'s numeric
  // operators (a state's readValue is arbitrary text, e.g. "high", not
  // necessarily a number). -1 if no topic is set, no state matches, or no
  // retained value has arrived yet.
  //
  // Public since 2026-08-25: main.cpp's tap handler needs it to work out
  // which state a single-area Switch should advance to. Recomputing "which
  // state is active" over there would be a second copy of the rule, and the
  // segment-width comment in dispatchTapAt already records what happens
  // when the drawing and the hit-testing disagree about the same geometry.
 public:
  int getActiveSwitchStateIndex(const ScreenObject& obj) const;

 private:

  void drawTextBox(const ScreenObject& obj, const String& displayText, bool drawBorder = true);

  // Line-geometry helpers, ported near-verbatim from schaltli-eink's
  // ScreenRenderer (see that class's own comments for the full derivation
  // of each - fillet tangent/arc math, thick-line rasterization via a
  // rotated-rectangle inside-test, arrowhead placement/shortening). Shared
  // by renderLine() (fixed arrowStart/arrowEnd) and renderMqttDataLine()
  // (data-driven arrow conditions, data-driven stroke width) - identical to
  // how the e-paper reference shares them between its own two callers.

  // Fills a strokeWidth>1 line as a rotated rectangle with butt caps.
  void fillThickLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, float width, uint16_t color);

  // Rounds every interior vertex of a multi-point line via a true circular
  // arc between tangent points (not a Bezier approximation - see the e-paper
  // reference's 2026-07-30 finding for why a Bezier looked visibly wrong at
  // sharp angles). `drawSegment` is whatever renderLine()/renderMqttDataLine()
  // would otherwise call directly per straight segment.
  void renderFilletedLine(const std::vector<Point>& points, int filletRadius,
                           const std::function<void(int16_t, int16_t, int16_t, int16_t)>& drawSegment);

  // Straight segments when filletRadius<=0 or too few points, else
  // delegates to renderFilletedLine().
  void renderLineBody(const std::vector<Point>& points, int filletRadius,
                       const std::function<void(int16_t, int16_t, int16_t, int16_t)>& drawSegment);

  // A filled-triangle arrowhead pointing from `from` toward `tip`.
  void drawArrowhead(const Point& tip, const Point& from, float strokeWidth, uint16_t color);

  // The point a strokeWidth-wide line body should actually stop at when
  // drawing toward an end that shows an arrowhead - short of the true tip,
  // so the body's straight edges don't poke out past the arrowhead's
  // tapering sides.
  Point shortenForArrow(const Point& tip, const Point& from, float strokeWidth);

  // The font a project object asks for, taken from the .bdf this device
  // publishes in its own DDF. Falls back to the DDF's first font when a
  // project names one this device does not declare; nullptr only if the
  // DDF itself is unreadable, which is a broken build.
  const BdfFont* fontFor(const String& fontId);

  // How far the clip box has to grow upward so this text's ink survives.
  // The baseline sits at y + ascent, but ink can reach above the nominal
  // ascent - capital umlauts do, by one pixel in helvR18 - and clipping to
  // the nominal box shaved the dots off Ä Ö Ü.
  //
  // Measured from the string, not the font: helvR18's font-wide maximum is
  // 29 against a nominal 22, from some stacked diacritic in Latin
  // Extended, so using that would grow every label's box by seven pixels
  // including pure ASCII ones that overhang by nothing.
  int16_t inkOverhang(const BdfFont* font, const String& text, int16_t nominalAscent) const;
  int getFontAscentById(const String& fontId) const;
  int getFontDescentById(const String& fontId) const;
  int getFontSizeById(const String& fontId) const;
  int16_t measureTrueTextWidth(const String& text, const BdfFont* font);
  String formatNumber(const String& valueStr, int decimals, const String& thousandsSeparator) const;
  float interpolateCalibration(float value, const std::vector<CalibrationPoint>& points) const;
  // Keyed by the topic the request is about, not by the object that took it:
  // two bars on one dimmer are one value, and both show what was asked of it.
  String askedTopic_;
  String askedValue_;
  float calibrationValueForPercent(float percent, const std::vector<CalibrationPoint>& points) const;

 public:
  // A level a finger can set: one with somewhere to write to
  // (docs/2026-09-17-settable-level.md in the designer repo, decision 1).
  // Only the bar so far - the ring's own sector arithmetic follows.
  static bool isSettableLevel(const ScreenObject& obj);

  // What a finger at this point means for that bar: the position as a
  // percentage of the bar, through the calibration read backwards, snapped
  // to the object's step. x/y are absolute screen coordinates, objX/objY the
  // object's own corner as hitTestTappable reports it.
  float settableLevelValueAt(const ScreenObject& obj, int16_t x, int16_t y, int16_t objX, int16_t objY) const;

  // The object with this id on that screen, with its absolute corner - the
  // same walk and the same coordinates hitTestTappable reports, for a finger
  // that has left the object it started on: a drag past the end of a bar
  // still means the end of that bar, not "nothing here".
  const ScreenObject* findObjectById(int screenIndex, const String& id, int16_t& objX, int16_t& objY) const;

  // «Adjust a slider or dial» (docs/device-contract.md §5 in the designer
  // repo, 2026-10-04): the slider or dial a press moves - the target itself,
  // or for a switcher the first settable level, in object order, of the panel
  // it shows now. nullptr when the target is not on screen or its panel holds
  // none: the press does nothing.
  const ScreenObject* adjustableLevel(int screenIndex, const String& targetId) const;

  // The value one press writes, from the value shown (`current`): one step up
  // or down onto the grid a finger snaps to (roundf(value / step) * step),
  // clamped to the outer calibration points (default 0-100). False at the end
  // of the range, and while no value is known yet (after a restart, before
  // the installation said anything) - nothing to write: stepping a heater
  // from a guessed minimum would surprise more than a ring that waits
  // (decided 2026-10-04).
  // The designer's lib/adjust-level.ts adjustedValue() does the same.
  static bool adjustedLevelValue(const ScreenObject& level, const String& current, bool up, float& next);

  // What a finger asked one object for, drawn as its setpoint marker until
  // the installation answers (docs/2026-09-17-settable-level.md in the
  // designer repo, decision 6c: the finger moves the marker, the fill keeps
  // showing what is measured, and the two coincide once the command landed).
  //
  // Not a topic: a dimmer's level is one value on the broker with no second
  // topic to hold "asked for", so the device remembers it - and forgets it
  // the moment a value arrives on that object's read topic, which is where
  // each board's main.cpp clears it. A setpoint topic, where there is one,
  // simply wins: the installation's own word about the target beats this.
  void setAskedValue(const String& markerTopic, const String& value);

  // The rectangle every object showing this marker topic lives in, as one box.
  // A board that redraws regions rather than screens needs it: a request is
  // about a value, so two bars on one dimmer both change, and redrawing only
  // the one under the finger leaves the other showing the old marker (found on
  // the 4.3B, 166 differing pixels in the conformance photograph).
  bool markerTopicBounds(int screenIndex, const String& markerTopic, int16_t& x, int16_t& y, int16_t& w,
                         int16_t& h) const;

  // What a finger moving this marker topic from oldValue to newValue changes,
  // as separate rectangles rather than one box round everything bound to it:
  // a slider across the top and a dial at the bottom on one dimmer made that
  // box the whole screen, 713 ms a drag frame on the 4.3B (2026-09-28). A ring
  // gives the stretch of its band between the two values and the numbers in
  // its middle; anything else its own rectangle. Returns how many were
  // written, or -1 where it cannot say (an object inside a switcher, more than
  // `max`) - the caller then falls back to markerTopicBounds.
  int levelDragRects(int screenIndex, const String& markerTopic, const String& oldValue, const String& newValue,
                     int16_t* rx, int16_t* ry, int16_t* rw, int16_t* rh, int max);
  // The value this marker topic is showing as asked for, or "" when none is.
  String askedValueFor(const String& markerTopic) const {
    return askedTopic_ == markerTopic ? askedValue_ : String();
  }
  void clearAskedValue();
  // What a level shows: the report, or on the dimmer pattern the finger (see the .cpp).
  String shownLevelValue(const ScreenObject& obj) const;

 private:

  // Parses "#rrggbb" (or "#rrggbbaa", alpha ignored except to detect
  // full transparency) into a real RGB565 value - unlike the e-paper
  // firmware's parseColor(), which only ever needed to decide black vs.
  // white. "transparent"/"#ffffff00"/similar all resolve to isTransparent
  // = true via the out-param, matching drawTextBox's existing
  // transparent-detection conventions (see its own comment for the exact
  // string set this mirrors).
  uint16_t parseHexColor(const String& colorStr, bool* isTransparent = nullptr) const;
};
