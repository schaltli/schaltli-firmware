#pragma once
#include <Arduino.h>
#include <Adafruit_GFX.h>
#include "BdfFont.h"
#include "DdfFontStore.h"
#include "../interfaces/IProjectLoader.h"
#include "ProjectTypes.h"
#include "../ClippedCanvas16.h"
#include "ColorAssetLoader.h"
#include <functional>

// Color (RGB565) counterpart of MqttEPaperDisplay2's ScreenRenderer -
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
// MqttEPaperDisplay2 never implemented either (no touchscreen, and its own
// "icon" object type was never wired up despite existing in the schema) -
// so their rendering (draw the already-exported bitmap via ColorAssetLoader,
// always the "normal" state for SoftwareButton until real touch tracking
// exists) was designed fresh from the designer's own export shape
// (lib/project-zip.ts's path/pathNormal/pathActive fields), not ported from
// existing firmware logic.
//
// tab-control/panel/MqttDataLine (2026-08-14 port from MqttEPaperDisplay2's
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
  DdfFontStore fontStore_;

  bool renderObject(const ScreenObject& obj, const String& pressedButtonId, const String& pendingSwitchId,
                     int pendingSwitchStateIndex, const String& pressedSwitchId, int pressedSwitchStateIndex);
  bool renderBox(const ScreenObject& obj);
  bool renderLine(const ScreenObject& obj);
  bool renderLabel(const ScreenObject& obj);
  bool renderMQTTDataField(const ScreenObject& obj);
  bool renderLevelIndicator(const ScreenObject& obj);
  // The round counterpart of the bar. `mixInto` is what its anti-aliased
  // edges blend against where the object's own background is transparent -
  // a declared colour rather than whatever happens to be on the canvas,
  // because the designer cannot read its canvas back at an arbitrary zoom
  // and an edge that mixes into different things on the two sides is an
  // edge that fails the pixel comparison.
  bool renderArcLevel(const ScreenObject& obj, uint16_t mixInto);

  // The background renderScreen() filled before drawing any object.
  uint16_t currentScreenBg_ = 0xFFFF;
  bool renderMQTTIconField(const ScreenObject& obj);
  bool renderIcon(const ScreenObject& obj);
  bool renderSoftwareButton(const ScreenObject& obj, bool isPressed);
  bool renderSwitch(const ScreenObject& obj, int pendingStateIndex, int pressedStateIndex);
  // Switch marker bar - solid for a confirmed marked state, hollow while a
  // write is unconfirmed. Geometry constants live next to the definition in
  // ColorScreenRenderer.cpp, mirroring render-switch.ts.
  void drawSwitchBar(int x, int y, int width, uint16_t color, bool hollow);
  // One Switch state's icon + label inside the content band, shared by both
  // modes so a state cannot look different depending on which drew it.
  // x/width are floats because a switch segment's own boundaries need not
  // be whole pixels - see renderSwitch()'s segment loop.
  void drawSwitchStateContent(const SwitchState& state, const String& iconPath,
                              float x, float width, int bandTop, int bandHeight,
                              uint16_t textColor, const BdfFont* font,
                              int16_t fontAscent, int16_t fontDescent);
  bool renderMqttDataLine(const ScreenObject& obj);

  // Recurses into the active panel's children (offset-copies with
  // obj.x/obj.y added, since child coordinates are relative to the
  // tab-control's own origin) via renderObject() - draws nothing itself if
  // no panel matches. Mirrors MqttEPaperDisplay2's ScreenRenderer::
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
  // MqttEPaperDisplay2's ScreenRenderer::getActivePanel() exactly.
  const ScreenObject* getActivePanel(const ScreenObject& tabControl) const;

  bool evaluateCondition(float value, const String& op, float threshold) const;

  // Generalization of evaluateCondition() for panel matching: "==" / "!="
  // compare as trimmed Strings (an enum mode like "TEMP" isn't numeric, so
  // the float-only evaluateCondition() can't distinguish "TEMP" from
  // "POWER" - both toFloat() to 0.0), the four numeric operators delegate
  // to the existing evaluateCondition() via .toFloat() on both sides.
  // Mirrors MqttEPaperDisplay2's ScreenRenderer::evaluateVisibilityCondition()
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

  // Line-geometry helpers, ported near-verbatim from MqttEPaperDisplay2's
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
  void clearAskedValue();

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
