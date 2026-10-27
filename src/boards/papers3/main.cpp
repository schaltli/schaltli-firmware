// M5Stack PaperS3 - board bring-up.
//
// Third board in this repo, started 2026-09-13. The decisions it is being
// built to are in the designer repo's docs/2026-09-13-papers3-grilling.md,
// cited by number where they bite.
//
// What is here: the panel, the project stack above it, the test interface
// that lets a conformance run compare this device against the designer pixel
// for pixel, and touch - taps on Switch and SoftwareButton, plus the
// ten-second hold that opens setup mode.
//
// Almost none of the code below is this board's. ProjectLoader,
// ColorScreenRenderer, MqttClient, DeployManager and TestInterfaceServer
// already run on two boards; this file is the wiring and the parts that are
// genuinely different, which is the panel (EpdPanel.h) and what that implies
// for when to draw.

#include "project/NavigatorLayout.h"
#include "project/ScreenGraphProject.h"
#include <Arduino.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <esp_heap_caps.h>

#include "DeviceInfo.h"
#include "EpdPanel.h"
#include "PanelDisplayAdapter.h"
#include "../../ClippedCanvas16.h"
#include "../../DeployManager.h"
#include "../../DeviceProtocol.h"
#include "../../MqttClient.h"
#include "../../NvsConfigStorage.h"
#include "../../TestInterfaceServer.h"
#include "../../DeviceRequests.h"
#include "../../TestInterfaceSwitch.h"
#include "../../WiFiSupervisor.h"
#include "../../BootReport.h"
#include "../../FirmwareImage.h"
#include "../../FirmwareUpdater.h"
#include "../../WiFiSetupServer.h"
#include "../../project/ColorScreenRenderer.h"
#include "../../project/SwitchShape.h"
#include "../../project/Keyboard.h"
#include "../../DeviceSettings.h"
#include "../../project/ProjectLoader.h"
#include "../../project/ThemeVariant.h"
#include "project/ObjectTypes.h"

// The panel's own size, read back at boot and checked against what the DDF
// declares. A mismatch means the designer is laying out screens for a panel
// this device does not have, and every pixel comparison afterwards would be
// measuring that instead of whatever it was meant to measure.
static const int16_t DDF_SCREEN_W = 960;
static const int16_t DDF_SCREEN_H = 540;

static EpdPanel panel;
static ClippedCanvas16 *canvas = nullptr;
static PanelDisplayAdapter *displayAdapter = nullptr;

static ProjectLoader projectLoader;
static ColorScreenRenderer *renderer = nullptr;
static int currentScreen = 0;
// The popup open over currentScreen, as an index after the screens
// (screenOrPopupAt, ProjectTypes.h), or -1 (designer docs/device-contract.md
// 2.5). This board's own state, never on MQTT.
static int openPopup = -1;
static String lastPopup = "none since boot";
// The screen or popup a finger acts on now.
static int activeScreen() { return openPopup >= 0 ? openPopup : currentScreen; }
// How an open popup stands out on e-ink, where a grey wash over the screen
// would cost a full refresh and look dirty in 16 levels: a hard black shadow
// down and to the right, and a black edge. The screen underneath stays as it
// is (designer docs/device-contract.md 2.5).
static const int POPUP_SHADOW_PX = 10;
static const int POPUP_EDGE_PX = 4;

static NvsConfigStorage configStorage;
static WiFiSetupServer *wifiSetupServer = nullptr;
static bool setupModeActive = false;

static MqttClient mqttClient;
static DeviceProtocol protocol(mqttClient);
// ddf/get and recovery/get, answered over the broker (DeviceRequests.h).
static DeviceRequests deviceRequests(mqttClient);
// hil-on <password> / hil-off over USB (TestInterfaceSwitch.h).
static TestInterfaceSwitch testInterfaceSwitch;
static DeployManager deployManager;
static FirmwareUpdater firmwareUpdater;
// A deploy trigger, held from the MQTT callback until loop() runs it.
// DeployManager::handleDeployMessage() must not run inside the callback:
// its status publishes share PubSubClient's buffer with the packet being
// parsed (DeployManager.h). This board did exactly that until 2026-09-15.
// hil/deploy-check.js passed against it all the same, with ~16 publishes
// per deploy, but the M5 Dial lost status that way at ~100, and the rule
// the class states is not one to keep only while it happens to hold.
static bool pendingDeploy = false;
static String pendingDeployPayload;
static TestInterfaceServer testInterface;
static bool topicsSubscribed = false;

// Set when a topic value lands, cleared when the screen is drawn. The draw
// never happens inside the MQTT callback: on this panel it costs the better
// part of a second, and doing that inside the network loop blocks everything
// else for exactly as long. The value is stored the moment it arrives; only
// the drawing waits, and loop() decides when.
static bool renderPending = false;

// --- touch ------------------------------------------------------------
//
// The panel's GT911 comes with M5Unified, so unlike the 4.3B there is no
// driver to port - M5.update() fills M5.Touch and the code below only has to
// decide what a contact means.
//
// Injection exists so a test can press without a finger: POST /api/touch
// sets a point and a short window during which it wins over the glass. A
// posted lift ends the window at once rather than extending it, so the
// release runs against a panel that is also reading idle.
static const uint32_t TOUCH_INJECT_HOLD_MS = 2000;
static int16_t injectX = 0, injectY = 0;
static bool injectDown = false;
static uint32_t injectUntilMs = 0;
static bool injectLiftOnce = false;

// The setup-mode gesture (decision 7): ten seconds held in the top left
// quadrant. Long on purpose - 0.3s of feedback latency trains people to
// press longer, so the 4.3B's two seconds would fire here by accident.
static const uint32_t SETUP_HOLD_MS = 10000;
// When the countdown box appears. Not at once like the 4.3B's second: this
// panel answers a press late, so people press longer, and a button held for
// two seconds in the top left should not throw a box over the screen. Three
// seconds, the user's choice (2026-09-15).
static const uint32_t SETUP_HOLD_FEEDBACK_MS = 3000;
static bool holdActive = false;
static uint32_t holdStartMs = 0;
static int holdLastSecondShown = -1;
// Set once the countdown box is on the glass. From then on the contact is
// the setup hold and nothing else: its release presses nothing and swipes
// nothing, and puts the project back instead.
static bool holdCountdownShown = false;
// Where the contact began and where it is now. A tap acts on the first, a
// swipe on the difference, and a hold needs to know it has not become either.
// Das Topic, dessen naechste Nachricht eine Anforderung beendet - das
// Sollwert-Topic, wo es eines gibt, sonst das Lese-Topic (Entscheidung 6c).
static String levelAskedTopic;

// The keyboard a tapped text field opened (Keyboard.h, designer
// tasks/ha-text-plan.md): the keys over the lower part, a key acted on at
// its press and only the keyboard's rectangle refreshed, fast. While it is
// open every contact is its own and arriving values wait for it to close.
static bool keyboardOpen = false;
static keyboard::State keyboardState;
static keyboard::Frame keyboardFrame;
static ScreenObject keyboardField;
// From a key's press to its letter pushed to the glass, the last one - what
// a typist waits for on e-ink (the plan promises it measured).
static uint32_t keyboardLastKeyMs = 0;
static int16_t contactStartX = 0, contactStartY = 0;
static int16_t contactLastX = 0, contactLastY = 0;
static bool contactMoved = false;

// Past a jittery finger, and clearly one axis rather than a diagonal. Same
// numbers as the 4.3B's, which were settled against real hands.
static const int SWIPE_MIN_DISTANCE = 60;
static const int SWIPE_AXIS_RATIO = 2;
// A hold stops being a hold once the finger travels. Without this a slow
// drag that began in the top left would still open setup mode.
static const int HOLD_MOVE_CANCEL = 20;

// One status line every ten seconds, because on this board the boot log is
// genuinely hard to catch: its USB device re-enumerates on every reset, so a
// serial capture started beforehand ends up holding a handle to a device
// that no longer exists and reads nothing, while one started afterwards has
// already missed setup(). Rather than choreograph that, the device just says
// where it is, forever, and any capture started at any moment finds out.
static void heartbeat()
{
  static uint32_t lastMs = 0;
  if (millis() - lastMs < 10000) return;
  lastMs = millis();
  Serial.printf("[papers3] up %lus | wifi %s %s | mqtt %s | project %s | screen %d | partials %u/%u\n",
                (unsigned long)(millis() / 1000),
                WiFi.status() == WL_CONNECTED ? "ok" : "down",
                WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "-",
                mqttClient.isConnected() ? "ok" : "down",
                projectLoader.isLoaded() ? "loaded" : "none", currentScreen,
                (unsigned)panel.partialsSinceFull(), (unsigned)EpdPanel::PARTIALS_BEFORE_CLEANUP);
}

static void enterSetupMode();

// Which screens are hidden (designer device contract 2.7): out of paging,
// never the start, still a goto target.
static std::vector<bool> hiddenFlags()
{
  std::vector<bool> hidden;
  if (!projectLoader.isLoaded()) return hidden;
  for (const Screen &screen : projectLoader.getProject().screens) hidden.push_back(screen.hidden);
  return hidden;
}

// The screen's navigator, if the point lies on its strip (2.7).
static bool onNavigatorStrip(int16_t x, int16_t y, navigatorlayout::Layout &layout)
{
  if (!renderer || openPopup >= 0 || !renderer->navigatorLayout(currentScreen, layout)) return false;
  const navigatorlayout::Rect &s = layout.bar;
  return x >= s.x && y >= s.y && x < s.x + s.width && y < s.y + s.height;
}

// The screen the navigator last followed to: it follows a screen change
// only, never a redraw of the same screen - which would undo a scroll the
// finger just made (found on the PaperS3, 2026-10-08).
static int navigatorFollowedScreen = -1;

// Before `target` is drawn: the navigator scrolled as far as shows its
// entry, from where it is now. Untouched (-1) it does that by itself.
static void navigatorFollow(int target)
{
  if (target == navigatorFollowedScreen) return;
  navigatorFollowedScreen = target;
  if (!renderer || renderer->navigatorScroll() < 0) return;
  navigatorlayout::Layout layout;
  int active = -1;
  if (!renderer->navigatorLayout(target, layout, nullptr, &active) || active < 0) return;
  renderer->setNavigatorScroll(navigatorlayout::scrollToShow(layout, active, renderer->navigatorScroll()));
}

static void renderAndPresent(const char *reason)
{
  if (!renderer || !canvas) return;
  navigatorFollow(currentScreen);
  const uint32_t t0 = micros();
  bool ok = renderer->renderScreen(currentScreen);
  if (openPopup >= 0)
  {
    const PopupFence &f = projectLoader.getProject().popupFence;
    canvas->fillRect(f.x + POPUP_SHADOW_PX, f.y + POPUP_SHADOW_PX, f.width, f.height, 0x0000);
    ok = renderer->renderPopup(openPopup, 0) && ok;
    for (int i = 0; i < POPUP_EDGE_PX; i++) canvas->drawRect(f.x + i, f.y + i, f.width - 2 * i, f.height - 2 * i, 0x0000);
    // The close button again, over the edge just drawn across its corner.
    const Screen *popup = screenOrPopupAt(projectLoader.getProject(), openPopup);
    if (popup != nullptr) renderer->drawPopupCloseBadge(*popup);
  }
  if (keyboardOpen) renderer->renderKeyboard(keyboardState, keyboardField, keyboardFrame);
  const uint32_t t1 = micros();
  panel.blit(canvas->getBuffer());
  const uint32_t t2 = micros();
  renderPending = false;
  Serial.printf("[papers3] render(screen %d, %s): %s | draw %lums, panel %lums%s\n",
                currentScreen, reason, ok ? "ok" : "FAILED",
                (unsigned long)((t1 - t0) / 1000), (unsigned long)((t2 - t1) / 1000),
                panel.lastWasFull() ? " (full refresh)" : "");
}

// Which state a tap on a Switch asks for. Segment arithmetic identical to
// ColorScreenRenderer::renderSwitch()'s own division, because a finger and
// the picture disagreeing about where a segment ends is a bug nobody can
// see in a screenshot.
static int switchSegmentFor(const ScreenObject *hit, int16_t x, int16_t objX)
{
  // One rule, in switchshape::stateIndexForTap - the designer's own
  // switchStateIndexForTap, mirrored. In the switch form two states toggle
  // wherever they are hit, more take the slot under the finger (or advance,
  // beside the track); a group takes the segment. A second copy here would
  // drift, and a finger would hit something other than what it points at.
  const int states = (int)hit->properties.states.size();
  if (states <= 0) return -1;
  const int active = renderer == nullptr ? -1 : renderer->getActiveSwitchStateIndex(*hit);
  // In the object's own coordinates: a Switch inside a panel carries
  // coordinates relative to its tab-control, while the finger is in screen
  // ones. objX is where the hit test found the object, so this is the offset
  // between the two.
  const int localX = x - objX + hit->x;
  return switchshape::stateIndexForTap(*hit, localX, active);
}

static void closePopup(const char *reason);

// A project topic's new value, from the broker or - for a local topic - from
// a write on this display (writeProjectTopic, applied from loop()): kept, and
// drawn by the loop as an arriving message is.
static void applyTopicValue(const String &topic, const String &payload)
{
  // Was die Anlage meldet, beendet die Anforderung: der Marker geht, die
  // Fuellung zeigt den gemeldeten Wert (Entscheidung 6c).
  if (renderer && !levelAskedTopic.isEmpty() && topic == levelAskedTopic)
  {
    renderer->clearAskedValue();
    levelAskedTopic = "";
  }
  projectLoader.setTopicValue(topic, payload);
  renderPending = true;
}

// Local topics (designer contract 2.9): a write to one waits here for loop(),
// which applies it as an arriving message - after the level that wrote it
// has marked what it asked for, so the value answers that mark.
static std::vector<std::pair<String, String>> localWrites;

// Every write to a project topic: a local one is this display's value, any
// other goes to the broker - not retained, a command, not a state. True when
// it was taken.
static bool writeProjectTopic(const String &topic, const String &payload)
{
  if (projectLoader.isLoaded() && projectLoader.isLocalTopic(topic))
  {
    localWrites.push_back({topic, payload});
    return true;
  }
  return mqttClient.publish(topic, payload, false);
}

static void dispatchButtonAction(const ButtonAction &action)
{
  if (action.type.isEmpty() || !projectLoader.isLoaded()) return;
  const ProjectConfig &project = projectLoader.getProject();
  const int count = (int)project.screens.size();

  if (action.type == "next-screen" || action.type == "previous-screen")
  {
    if (count == 0) return;
    const int delta = (action.type == "next-screen") ? 1 : -1;
    // Over the main row (contract 2.10): from a screen off it, from its page.
    currentScreen = screengraphproject::rowStepIndex(project, currentScreen, delta);
    openPopup = -1;
    panel.forceFullNext();
    renderAndPresent("button: screen change");
  }
  else if (action.type == "goto-neighbour")
  {
    // The screen lying on that side (2.10); none there: nothing. E-paper
    // jumps - no slide.
    screengraph::Edge side;
    if (!screengraph::edgeFrom(action.side.c_str(), side)) return;
    const int target = screengraphproject::neighbourIndex(project, currentScreen, side);
    if (target < 0) return;
    currentScreen = target;
    openPopup = -1;
    panel.forceFullNext();
    renderAndPresent("button: neighbour");
  }
  else if (action.type == "goto-screen")
  {
    for (int i = 0; i < count; i++)
    {
      if (project.screens[i].id == action.targetScreenId)
      {
        currentScreen = i;
        openPopup = -1;
        panel.forceFullNext();
        renderAndPresent("button: goto screen");
        break;
      }
    }
  }
  else if (action.type == "send-mqtt")
  {
    // Not retained: a command, not a state. What comes back on the read
    // topic is what the panel then shows.
    writeProjectTopic(action.mqttTopic, action.mqttMessage);
  }
  else if (action.type == "open-popup")
  {
    // At once, no animation: every frame on this panel is a refresh of its
    // own. A fast partial paint, as any other answer to a tap.
    const int index = popupIndexById(project, action.targetScreenId);
    if (index < 0 || !project.popupFence.present)
    {
      Serial.printf("[papers3] no popup \"%s\" - skipping\n", action.targetScreenId.c_str());
      return;
    }
    openPopup = index;
    renderAndPresent("popup opened");
    lastPopup = "opened \"" + screenOrPopupAt(project, index)->name + "\"";
  }
  else if (action.type == "close-popup")
  {
    closePopup("button");
  }
}

// Back to the screen underneath, painted clean: a popup is a box over the
// screen, and its partial paint and the one taking it away leave a trace a
// full refresh wipes (as a screen change is painted, EpdPanel.h).
static void closePopup(const char *reason)
{
  if (openPopup < 0) return;
  openPopup = -1;
  panel.forceFullNext();
  renderAndPresent("popup closed");
  lastPopup = String("closed by ") + reason;
}

// A press at x,y. Returns true when it hit something that acted.
//
// The search runs through ColorScreenRenderer::hitTestTappable() rather than
// a loop over the screen's objects here, because the answer depends on two
// things that already live in the renderer: which panel of a tab-control is
// showing, and how far its children are drawn shifted. A second copy would
// mean the finger and the picture disagreeing about geometry, which is how
// the knob ended up with a working top-level switch and four dead ones
// inside panels.
static void presentKeyboard()
{
  renderer->renderKeyboard(keyboardState, keyboardField, keyboardFrame);
  panel.blitRegion(canvas->getBuffer(), keyboardFrame.x, keyboardFrame.y, keyboardFrame.w, keyboardFrame.h);
}

static void openKeyboard(const ScreenObject &field)
{
  keyboard::Layout layout = keyboard::Layout::EnUS;
  keyboard::parseLayout(DeviceSettings::keyboardLayoutOrDefault().c_str(), layout);
  keyboardField = field;
  keyboardState = keyboard::State();
  keyboardState.layout = layout;
  keyboardState.bounds = textfield::bounds(field.properties.minLength, field.properties.maxLength);
  String shown = renderer->askedValueFor(field.properties.topic);
  if (shown.isEmpty() && projectLoader.hasTopicValue(field.properties.topic)) shown = projectLoader.getTopicValue(field.properties.topic);
  keyboardState.text = textfield::clampText(shown.c_str(), keyboardState.bounds.maxLength);
  keyboardFrame = keyboard::frameFor(panel.width(), panel.height(), false);
  keyboardOpen = true;
  presentKeyboard();
}

// OK sends the text as it is, not retained, and the field shows it until
// its topic answers (the asked value, levelAskedTopic) - as on the 4.3B.
static void closeKeyboard(bool send)
{
  keyboardOpen = false;
  if (send)
  {
    const String text(keyboardState.text.c_str());
    writeProjectTopic(keyboardField.properties.writeTopic, text);
    levelAskedTopic = keyboardField.properties.topic;
    renderer->setAskedValue(levelAskedTopic, text);
    Serial.printf("[papers3] text field \"%s\" -> %s = %s\n", keyboardField.id.c_str(),
                  keyboardField.properties.writeTopic.c_str(), text.c_str());
  }
  // The keyboard leaves more of a trace than one partial's worth.
  renderAndPresent(send ? "keyboard sent" : "keyboard cancelled");
  panel.noteHeavyGhosting();
}

static void keyboardTouch(int16_t x, int16_t y)
{
  const uint32_t t0 = millis();
  const std::vector<keyboard::KeyRect> keys = keyboard::layoutKeys(
      keyboardState, keyboardFrame.keysX, keyboardFrame.keysY, keyboardFrame.keysW, keyboardFrame.keysH, keyboardFrame.gap);
  const keyboard::Key *key = keyboard::keyAt(keys, x, y);
  if (key == nullptr) return;
  const keyboard::Outcome outcome = keyboard::press(keyboardState, *key);
  if (outcome == keyboard::Outcome::Send) closeKeyboard(true);
  else if (outcome == keyboard::Outcome::Cancel) closeKeyboard(false);
  else
  {
    presentKeyboard();
    keyboardLastKeyMs = millis() - t0;
  }
}

static bool handleTap(int16_t x, int16_t y)
{
  if (!renderer || !projectLoader.isLoaded()) return false;

  // A tap on the navigator opens its entry's screen, as goto-screen does
  // (contract 2.7); its empty end takes the tap too.
  {
    navigatorlayout::Layout layout;
    const Navigator *nav = nullptr;
    if (openPopup < 0 && renderer->navigatorLayout(currentScreen, layout, &nav))
    {
      // Its light/dark switch asks for the other variant (2.7, generation
      // 1.6). E-paper draws light only; the other devices follow.
      const navigatorlayout::Rect &sw = layout.themeSwitch;
      if (layout.hasThemeSwitch && x >= sw.x && y >= sw.y && x < sw.x + sw.width && y < sw.y + sw.height)
      {
        const bool dark = themevariant::isDarkPayload(projectLoader.getTopicValue("schaltli/state/theme"));
        mqttClient.publish("schaltli/cmnd/theme", dark ? "light" : "dark", false);
        Serial.printf("[papers3] tap: theme switch -> %s\n", dark ? "light" : "dark");
        return true;
      }
      const int entry = navigatorlayout::entryAt(layout, x, y, renderer->effectiveNavigatorScroll(currentScreen));
      const navigatorlayout::Rect &s = layout.bar;
      if (entry >= 0)
      {
        // The entry's screen, or off the main row the way back (2.10).
        const String target = renderer->navigatorEntryScreenId(currentScreen, entry);
        if (!target.isEmpty() && target != projectLoader.getProject().screens[currentScreen].id)
        {
          ButtonAction go;
          go.type = "goto-screen";
          go.targetScreenId = target;
          dispatchButtonAction(go);
        }
        return true;
      }
      if (x >= s.x && y >= s.y && x < s.x + s.width && y < s.y + s.height) return true;
    }
  }

  int16_t objX = 0, objY = 0;
  const ScreenObject *hit = renderer->hitTestTappable(activeScreen(), x, y, objX, objY);
  if (hit == nullptr) return false;

  // A node of an energy diagram with an action (contract 2.8): the hit test
  // only lets a diagram catch a tap on one. As on the 4.3B.
  if (hit->type == "energy-diagram")
  {
    const ButtonAction *action = renderer->energyActionAt(*hit, x - objX, y - objY);
    if (action == nullptr) return false;
    Serial.printf("[papers3] tap: energy diagram \"%s\" action %s\n", hit->id.c_str(), action->type.c_str());
    const ButtonAction copy = *action;
    dispatchButtonAction(copy);
    return true;
  }

  if (hit->type == "text-field")
  {
    Serial.printf("[papers3] tap: text field \"%s\" - keyboard\n", hit->id.c_str());
    openKeyboard(*hit);
    return true;
  }

  if (hit->type == "button")
  {
    Serial.printf("[papers3] tap: button \"%s\"\n", hit->id.c_str());
    dispatchButtonAction(hit->properties.action);
    return true;
  }

  // A settable level: the tap sets the value at the point it landed, sends it,
  // and shows it as the object's marker (designer
  // docs/2026-09-17-settable-level.md, decision 6c) - the fill keeps showing
  // what the installation reports, so the two coincide once the command has
  // landed. A tap, not a drag: on e-ink every intermediate frame is a full
  // refresh of about a second, which is why this board follows no finger
  // anywhere.
  if (ColorScreenRenderer::isSettableLevel(*hit))
  {
    const float value = renderer->settableLevelValueAt(*hit, x, y, objX, objY);
    String payload;
    if (fabsf(value - roundf(value)) < 0.001f)
    {
      payload = String((long)roundf(value));
    }
    else
    {
      payload = String(value, 2);
      while (payload.endsWith("0")) payload.remove(payload.length() - 1);
      if (payload.endsWith(".")) payload.remove(payload.length() - 1);
    }
    Serial.printf("[papers3] tap: level \"%s\" -> %s = %s\n", hit->id.c_str(),
                  hit->properties.writeTopic.c_str(), payload.c_str());
    // Not retained: a command, not a state.
    String sent;
    if (levelWritePayload(hit->properties.writeFormat, payload, sent)) writeProjectTopic(hit->properties.writeTopic, sent);
    // One refresh for the marker, and the request ends when the installation
    // speaks about that same setpoint (onMqttMessage).
    levelAskedTopic = hit->properties.setpointTopic.isEmpty() ? hit->properties.topic
                                                              : hit->properties.setpointTopic;
    renderer->setAskedValue(levelAskedTopic, payload);
    renderAndPresent("level asked");
    return true;
  }

  const int states = (int)hit->properties.states.size();
  if (states <= 0 || hit->properties.writeTopic.isEmpty()) return false;

  const int index = switchSegmentFor(hit, x, objX);
  if (index < 0) return false;
  const String &value = hit->properties.states[index].writeValue;
  if (value.isEmpty()) return false;

  // Not retained: a command, not a state. The state comes back on the read
  // topic and colours the segment, so the switch shows what actually
  // happened rather than what it believed it was doing.
  const bool sent = writeProjectTopic(hit->properties.writeTopic, value);
  Serial.printf("[papers3] tap: Switch \"%s\" segment %d -> %s = %s%s\n", hit->id.c_str(), index,
                hit->properties.writeTopic.c_str(), value.c_str(),
                sent ? "" : "  (NOT published - broker away)");
  return true;
}

// A drag, resolved at release: the dominant axis names a button and the
// project says what that button does.
//
// No animated page turn, unlike the 4.3B's follow-swipe. On this panel every
// intermediate frame is its own refresh of a second or so, so a gesture that
// drags the outgoing screen along would take the better part of a minute and
// leave a trail of ghosts. The screen changes once, at release, with a clean
// paint.
static void dispatchSwipeAtRelease(int dx, int dy)
{
  if (!projectLoader.isLoaded()) return;

  const char *buttonId = nullptr;
  if (abs(dx) > abs(dy))
  {
    if (abs(dx) >= SWIPE_MIN_DISTANCE && abs(dx) >= abs(dy) * SWIPE_AXIS_RATIO)
      buttonId = dx > 0 ? "swipe-right" : "swipe-left";
  }
  else
  {
    if (abs(dy) >= SWIPE_MIN_DISTANCE && abs(dy) >= abs(dx) * SWIPE_AXIS_RATIO)
      buttonId = dy > 0 ? "swipe-down" : "swipe-up";
  }
  if (buttonId == nullptr) return;

  // Any swipe closes an open popup and does nothing else (designer
  // docs/device-contract.md 2.5); one that began on a level never gets here.
  if (openPopup >= 0)
  {
    closePopup("swipe");
    return;
  }

  const ButtonAction action = projectLoader.getButtonAction(currentScreen, buttonId);
  if (action.type.isEmpty())
  {
    Serial.printf("[papers3] swipe: %s, nothing bound on this screen\n", buttonId);
    return;
  }
  Serial.printf("[papers3] swipe: %s -> %s\n", buttonId, action.type.c_str());
  dispatchButtonAction(action);
}

// One touch sample, from the glass or from a test, and the caller cannot
// tell which.
static bool readTouch(int16_t &x, int16_t &y)
{
  if (injectLiftOnce)
  {
    injectLiftOnce = false;
    return false;
  }
  if (millis() < injectUntilMs)
  {
    x = injectX;
    y = injectY;
    return injectDown;
  }

  const auto &d = M5.Touch.getDetail();
  if (!d.isPressed()) return false;
  x = d.x;
  y = d.y;
  return true;
}

// Press, hold and release, plus the setup gesture.
//
// Acting on release rather than on contact: a hold that becomes setup mode
// must not also have fired whatever it was resting on.
// Did this contact start on a level a finger can set? Asked at release, from
// the point the finger went down, so a drag across such an object is not read
// as a swipe (decision 8).
static bool startedOnSettableLevel()
{
  if (!renderer || !projectLoader.isLoaded()) return false;
  int16_t objX = 0, objY = 0;
  const ScreenObject *hit = renderer->hitTestTappable(activeScreen(), contactStartX, contactStartY, objX, objY);
  return hit != nullptr && ColorScreenRenderer::isSettableLevel(*hit);
}

static void serviceTouch()
{
  int16_t x = 0, y = 0;
  const bool down = readTouch(x, y);
  if (down) panel.noteTouch();

  // The keyboard takes every contact while it is open, a key on its press:
  // no swipe, no setup hold.
  if (keyboardOpen)
  {
    if (down && !holdActive)
    {
      holdActive = true;
      keyboardTouch(x, y);
    }
    else if (!down)
    {
      holdActive = false;
    }
    return;
  }

  if (down && !holdActive)
  {
    holdActive = true;
    holdStartMs = millis();
    holdLastSecondShown = -1;
    contactStartX = contactLastX = x;
    contactStartY = contactLastY = y;
    contactMoved = false;
    return;
  }

  if (down && holdActive)
  {
    contactLastX = x;
    contactLastY = y;
    if (abs(x - contactStartX) > HOLD_MOVE_CANCEL || abs(y - contactStartY) > HOLD_MOVE_CANCEL)
      contactMoved = true;

    // Setup mode: ten seconds, still, in the top left quadrant (decisions 7
    // and 20). A gesture that has travelled is on its way to being a swipe
    // and must not also open setup.
    if (contactMoved) return;
    const bool inCorner = contactStartX < panel.width() / 2 && contactStartY < panel.height() / 2;
    if (!inCorner) return;

    const uint32_t held = millis() - holdStartMs;
    if (held >= SETUP_HOLD_MS)
    {
      holdActive = false;
      holdCountdownShown = false;
      enterSetupMode();
      return;
    }

    const int second = (int)(held / 1000);
    if (second != holdLastSecondShown && second > 0)
    {
      holdLastSecondShown = second;
      Serial.printf("[papers3] setup hold %d/%d s\n", second, (int)(SETUP_HOLD_MS / 1000));
      // Until 2026-09-15 this was all there was, and ten seconds with nothing
      // on the glass read as a gesture that did not work. The box cannot
      // trigger a clean-up flash mid-hold: the finger resting on the glass
      // keeps the panel from ever counting as idle (EpdPanel.h).
      if (held >= SETUP_HOLD_FEEDBACK_MS && displayAdapter)
      {
        holdCountdownShown = true;
        displayAdapter->showHoldCountdown((int)((SETUP_HOLD_MS - held + 999) / 1000));
      }
    }
    return;
  }

  if (!down && holdActive)
  {
    holdActive = false;
    const int dx = contactLastX - contactStartX;
    const int dy = contactLastY - contactStartY;

    // A hold that got as far as its countdown was the setup hold, let go of.
    // It presses nothing and swipes nothing - the 4.3B's rule - and the
    // project goes back over the box, partially like every other redraw.
    // The box leaves more of a trace than one partial's worth, so the panel
    // is told to clean itself once nobody has touched it for ten seconds.
    if (holdCountdownShown)
    {
      holdCountdownShown = false;
      renderAndPresent("hold released");
      panel.noteHeavyGhosting();
      return;
    }

    // A still tap on an open popup's close button closes it, before anything
    // under the button - a level included - is asked.
    if (!contactMoved && (millis() - holdStartMs) < SETUP_HOLD_MS && openPopup >= 0 && renderer &&
        renderer->popupCloseBadge().contains(contactStartX, contactStartY))
    {
      closePopup("close button");
      return;
    }

    // A gesture that began on the navigator is its own (contract 2.7): a
    // swipe along it scrolls a page of whole entries - this board acts on
    // release and follows no finger - and never pages; a still one is a tap,
    // which handleTap below gives it.
    {
      navigatorlayout::Layout layout;
      if (contactMoved && onNavigatorStrip(contactStartX, contactStartY, layout))
      {
        const int along = layout.horizontal ? dx : dy;
        const int scroll = renderer->effectiveNavigatorScroll(currentScreen);
        const int next = navigatorlayout::pageScroll(layout, scroll, along < 0 ? 1 : -1);
        if (next != scroll)
        {
          renderer->setNavigatorScroll(next);
          renderAndPresent("navigator scroll");
        }
        return;
      }
    }

    // A gesture that began on a settable level belongs to it, however far it
    // travelled (designer docs/2026-09-17-settable-level.md, decisions 8 and
    // 9): this board follows no finger, so it takes the position the finger
    // lifted at - which is what someone dragging across a bar means by it -
    // and publishes once.
    if (startedOnSettableLevel())
    {
      handleTap(contactLastX, contactLastY);
      return;
    }

    // A travelled gesture is a swipe and never also a tap on whatever it
    // started over. A still one is a tap, provided it was not the setup
    // hold, which has already acted by now.
    if (contactMoved)
    {
      dispatchSwipeAtRelease(dx, dy);
      return;
    }
    if ((millis() - holdStartMs) >= SETUP_HOLD_MS) return;
    // A tap beside an open popup closes it and does nothing else.
    if (openPopup >= 0 && !projectLoader.getProject().popupFence.contains(contactStartX, contactStartY))
    {
      closePopup("tap outside");
      return;
    }
    handleTap(contactStartX, contactStartY);
  }
}

static void subscribeToProjectTopics()
{
  if (topicsSubscribed || !projectLoader.isLoaded() || !mqttClient.isConnected()) return;
  std::vector<String> topics;
  // A local topic is the display's own and never subscribed (contract 2.9).
  for (const auto &t : projectLoader.getProject().topics)
    if (!t.local) topics.push_back(t.topic);
  if (!topics.empty()) mqttClient.subscribeToKeys(topics);
  topicsSubscribed = true;
  Serial.printf("[papers3] subscribed to %d project topic(s)\n", (int)topics.size());
}

static bool setupPanelAndProject()
{
  if (!panel.begin())
  {
    Serial.println("[papers3] panel did not come up - nothing else can work");
    return false;
  }
  Serial.printf("[papers3] panel %dx%d\n", panel.width(), panel.height());

  if (panel.width() != DDF_SCREEN_W || panel.height() != DDF_SCREEN_H)
  {
    // Loud rather than adaptive. Taking the panel's word for it would let
    // this device render happily at a size the designer never laid out for,
    // and the comparison would then fail on dimensions with no hint that the
    // DDF is the thing that is wrong.
    Serial.printf("[papers3] DDF declares %dx%d - fix ddf-source-papers3/device.json\n",
                  DDF_SCREEN_W, DDF_SCREEN_H);
  }

  // ~1MB at 960x540x2. Internal SRAM has nothing like that free, so a
  // non-null buffer is itself the proof it landed in PSRAM - but report
  // where, because "it worked" and "it worked for the reason I assumed" are
  // different claims.
  canvas = new ClippedCanvas16(panel.width(), panel.height());
  if (!canvas || !canvas->getBuffer())
  {
    Serial.printf("[papers3] FATAL: %u-byte canvas allocation failed - is PSRAM on?\n",
                  (unsigned)((int32_t)panel.width() * panel.height() * 2));
    return false;
  }
  Serial.printf("[papers3] canvas at %p (%s), psram free %u\n", canvas->getBuffer(),
                esp_ptr_external_ram(canvas->getBuffer()) ? "PSRAM" : "internal SRAM",
                (unsigned)ESP.getFreePsram());

  displayAdapter = new PanelDisplayAdapter(*canvas, panel, panel.width(), panel.height());
  renderer = new ColorScreenRenderer(projectLoader, canvas);
  // Paper animates nothing: the energy diagram stands, dots at phase 0.
  renderer->setAnimates(false);
  // {device:id} in a text (designer docs/2026-10-05-placeholder-devices.md).
  // The id is fixed at construction, so it is there before the first draw.
  renderer->setDeviceId(mqttClient.getClientId());
  // An open popup shows a close button on its window's top right corner
  // (designer docs/device-contract.md 2.5). 52 px across, about 5.6 mm here;
  // the DDF says the same (screen.popupCloseRadius).
  renderer->setPopupCloseBadgeRadius(26);

  if (!LittleFS.begin(true))
  {
    Serial.println("[papers3] LittleFS mount failed");
    displayAdapter->showLines({"Dateisystem defekt"});
    return false;
  }

  if (!projectLoader.loadProject())
  {
    // Not a failure of this function: a device with no project yet is the
    // normal state of a board that has just been flashed, and it still has
    // to reach WiFi so one can be deployed to it.
    Serial.println("[papers3] no project installed yet");
    displayAdapter->showLines({"Schaltli", "PaperS3", "", "kein Projekt installiert"});
    return true;
  }

  // The first screen of the main row (contract 2.10; before it, the first
  // screen that is not hidden, which is what an older export's row is).
  currentScreen = screengraphproject::startIndex(projectLoader.getProject());
  Serial.printf("[papers3] project \"%s\" %dx%d, %u screen(s)\n",
                projectLoader.getProject().name.c_str(), projectLoader.getProject().screenWidth,
                projectLoader.getProject().screenHeight,
                (unsigned)projectLoader.getProject().screens.size());
  renderAndPresent("boot");
  return true;
}

static void enterSetupMode()
{
  if (setupModeActive || !wifiSetupServer) return;
  Serial.println("[papers3] entering setup mode - starting AP");
  setupModeActive = true;
  // The test interface owns port 80 in normal operation and the setup server
  // wants the same port, so it has to be stopped rather than left to fail a
  // bind nothing checks.
  testInterface.stop();
  wifiSetupServer->startAP();
}

// A late WiFi on demand, for hil/late-wifi.js - the same hook, for the same
// reason, as the 4.3B's (see there). RTC_NOINIT survives ESP.restart() and
// nothing else, so a power cycle can never inherit it.
static const uint32_t LATE_WIFI_MAGIC = 0x1A7E3F1F;
RTC_NOINIT_ATTR uint32_t lateWifiMagic;
RTC_NOINIT_ATTR uint32_t lateWifiSeconds;
static uint32_t lateWifiBootSeconds = 0;
static uint32_t restartAtMs = 0;
static WiFiSupervisor wifi;
static bool noWifiScreenShown = false;

static void setupNetworking()
{
  if (!canvas || !displayAdapter) return;

  configStorage.begin();
  wifiSetupServer = new WiFiSetupServer(*displayAdapter, configStorage);

  WiFiCredentials creds;
  if (!configStorage.loadWiFiCredentials(creds) || creds.ssid.isEmpty())
  {
    Serial.println("[papers3] no WiFi credentials - starting setup AP");
    enterSetupMode();
    return;
  }

  Serial.printf("[papers3] connecting to \"%s\"\n", creds.ssid.c_str());
  // WiFiSupervisor turns the radio's power save off, for the reason and
  // measurement recorded in the 4.3B's main.cpp. Mains-powered (decision 5),
  // so the 20-40mA it costs buys a panel that answers without waiting.
  uint32_t holdBackMs = 0;
  if (lateWifiMagic == LATE_WIFI_MAGIC)
  {
    lateWifiMagic = 0;
    lateWifiBootSeconds = lateWifiSeconds;
    holdBackMs = lateWifiSeconds * 1000UL;
    Serial.printf("[papers3] WiFi held back %us by ?set=latewifi\n", (unsigned)lateWifiSeconds);
  }
  wifi.begin(creds.ssid, creds.password, holdBackMs);
  for (int i = 0; i < 40 && !wifi.up(); i++)
  {
    wifi.tick();
    delay(250);
  }

  // A wait, not a verdict - everything below is set up either way. Returning
  // here left the 4.3B on the network with nothing listening until its next
  // restart (2026-09-14), and this board carried the same return.
  if (!wifi.up())
  {
    Serial.println("[papers3] no WiFi yet - carrying on, the supervisor keeps trying");
    displayAdapter->showLines({"WLAN nicht erreichbar", creds.ssid});
    noWifiScreenShown = true;
  }

  // The snapshot endpoint reads this canvas directly, which is what gives
  // this board a pixel comparison at all - and the reason ghosting is
  // invisible to it, since the glass is never what gets compared.
  if (testInterface.begin(*canvas)) Serial.println("[papers3] test interface on :80");

  testInterface.setScreenSwitchHandler([](int index) -> bool {
    if (!projectLoader.isLoaded()) return false;
    if (index < 0 || index >= (int)projectLoader.getProject().screens.size()) return false;
    currentScreen = index;
    openPopup = -1;
    // A screen change is a new picture, not a change to the old one, so it
    // gets a clean paint rather than spending the partial budget on a
    // rectangle that covers everything anyway.
    panel.forceFullNext();
    renderAndPresent("test interface: screen switch");
    return true;
  });

  // Lets a test ask "has the device applied this value yet" instead of
  // sleeping and hoping - the fix for a whole class of flaky runs.
  testInterface.setTopicValueHandler(
      [](const String &topic) { return projectLoader.getTopicValue(topic); });

  testInterface.setScreenListHandler([]() -> String {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    if (projectLoader.isLoaded())
    {
      for (const auto &screen : projectLoader.getProject().screens)
      {
        JsonObject o = arr.add<JsonObject>();
        o["id"] = screen.id;
        o["name"] = screen.name;
      }
    }
    String out;
    serializeJson(doc, out);
    return out;
  });

  // Lets a run press a Switch or a SoftwareButton without a finger, which
  // is what makes those two types testable at all (decision 11).
  testInterface.setTouchHandler([](int16_t x, int16_t y, bool down) {
    injectX = x;
    injectY = y;
    injectDown = down;
    injectUntilMs = down ? millis() + TOUCH_INJECT_HOLD_MS : 0;
    injectLiftOnce = !down;
  });

  testInterface.setKeyboardHandler([]() -> String {
    if (!keyboardOpen) return String("{\"open\":false}");
    // With the last key's time from press to glass beside what every board says.
    String json(keyboard::describe(keyboardState, keyboardFrame).c_str());
    json.replace("{\"open\":true,", "{\"open\":true,\"lastKeyMs\":" + String(keyboardLastKeyMs) + ",");
    return json;
  });
  testInterface.setDebugHandler([](const String &setting) -> String {
    String out = "ip " + WiFi.localIP().toString() + "  rssi " + String(WiFi.RSSI()) + " dBm\n";
    out += "heap " + String(ESP.getFreeHeap() / 1024) + " KB, psram free " +
           String(ESP.getFreePsram() / 1024) + " KB\n";
    out += "uptime " + String(millis() / 1000) + " s\n";
    out += String("wifi power save ") + (WiFi.getSleep() ? "on" : "none") + "\n";
    out += wifi.report();
    if (setting == "wifiscan=1") out += wifi.scanReport();
    if (lateWifiBootSeconds > 0)
      out += "wifi held back " + String(lateWifiBootSeconds) + " s at boot (?set=latewifi=<s>)\n";
    if (setting.startsWith("latewifi="))
    {
      lateWifiSeconds = (uint32_t)setting.substring(9).toInt();
      lateWifiMagic = LATE_WIFI_MAGIC;
      restartAtMs = millis() + 500;
      out += "restarting with WiFi held back " + String(lateWifiSeconds) + " s\n";
    }
    out += "firmware " + ESP.getSketchMD5() + ", built " + String(__DATE__) + " " +
           String(__TIME__) + "\n";
    out += BootReport::line();
    out += FirmwareImage::debugLine();
    // The refresh state, because decision 11 asks for this to be checked by
    // hand and no automated test can see it: the snapshot comes from the
    // canvas, not the glass.
    // ?set=repaint=1 redraws the current screen once, partially, exactly as a
    // topic value would. It is how hil/papers3/refresh-rule.js piles up
    // partials without a project, a broker or a finger.
    if (setting == "repaint=1") renderAndPresent("test interface: repaint");
    // ?set=setupscreen=<seconds left> draws the real AP setup screen into the
    // canvas without bringing the AP up, so /snapshot.bmp can show it -
    // setup mode itself stops this server. hil/papers3/setup-screen.js reads
    // the QR code back out of it. POST /api/screen puts the project back.
    if (setting.startsWith("setupscreen="))
      wifiSetupServer->showSetupScreenAP(setting.substring(12).toInt(), false);
    // ?set=holdcountdown=<seconds left> draws the setup hold's countdown box
    // over the current screen, so hil/papers3/hold-countdown.js can check
    // where it lands. Needed because a snapshot takes longer than an
    // injected touch lasts, so none can be taken during a real hold.
    // ?set=repaint=1 puts the screen back.
    if (setting.startsWith("holdcountdown=") && displayAdapter)
      displayAdapter->showHoldCountdown(setting.substring(14).toInt());
    out += "  popup: " + (openPopup >= 0 ? String("open, index ") + String(openPopup) : String("none open")) +
           " - last: " + lastPopup + "\n";
    out += "partials since last full refresh: " + String(panel.partialsSinceFull()) + " of " +
           String(EpdPanel::PARTIALS_BEFORE_CLEANUP) + ", last paint was " +
           String(panel.lastWasFull() ? "full" : "partial") + "\n";
    out += "last touch: " +
           (panel.touchedSinceBoot() ? String(panel.msSinceTouch()) + " ms ago" : String("never")) +
           ", clean-up after " + String(EpdPanel::IDLE_AFTER_TOUCH_MS) + " ms without one\n";
    // For hil/papers3/hold-countdown.js, which holds through POST /api/touch
    // and has to know when the box is up without reading it off a snapshot.
    out += "setup hold: " +
           (holdActive ? String(millis() - holdStartMs) + " ms held" : String("no contact")) +
           ", countdown " + (holdCountdownShown ? "showing" : "not showing") + "\n";
    return out;
  });

  if (creds.mqttHost.isEmpty())
  {
    Serial.println("[papers3] no broker configured - skipping MQTT");
    return;
  }

  mqttClient.configure(creds.mqttHost, (uint16_t)creds.mqttPort, creds.mqttUsername,
                       creds.mqttPassword);
  deployManager.setPublishStatusHandler(
      [](const String &id, const String &state, const String &error, int percent) {
        protocol.publishDeployStatus(id, state, error, percent);
      });
  deployManager.setClearTriggerHandler([]() { protocol.clearDeployTrigger(); });

  // Subscribed up front rather than from the connected callback: connect()
  // replays its subscription list on every reconnect, so queueing it once
  // covers every later connection too.
  mqttClient.subscribe(protocol.deviceTopic("deploy"));
  deviceRequests.subscribe();

  // Firmware over the air (FirmwareUpdater.h). The notice is one clean
  // paint, nothing more during the download - each progress step would be a
  // refresh of its own on this panel. A failed update puts the project back,
  // clean too, since the notice replaced the whole picture.
  firmwareUpdater.setPublishStatusHandler(
      [](const String &id, const String &state, const String &error, int percent) {
        protocol.publishDeployStatus(id, state, error, percent);
      });
  firmwareUpdater.setClearTriggerHandler([]() { protocol.clearFirmwareTrigger(); });
  firmwareUpdater.setStartHandler([]() {
    if (displayAdapter) displayAdapter->showLines({"Firmware-Update", "bitte nicht ausschalten"});
  });
  firmwareUpdater.setFailedHandler([]() {
    panel.forceFullNext();
    renderAndPresent("firmware update failed");
  });
  mqttClient.subscribe(protocol.deviceTopic("firmware"));
  mqttClient.setConnectedCallback([]() {
    protocol.publishHello();
    protocol.publishTestInterface(testInterface.isRunning() ? "http://" + WiFi.localIP().toString() : "");
    Serial.println("[papers3] published hello");
  });
  mqttClient.setCallback([](const String &topic, const String &payload) {
    if (deviceRequests.onMessage(topic)) return;
    if (topic == protocol.deviceTopic("firmware"))
    {
      firmwareUpdater.onMessage(payload);
      return;
    }
    if (topic == protocol.deviceTopic("deploy"))
    {
      pendingDeployPayload = payload;
      pendingDeploy = true;
      return;
    }
    // A real topic under a local topic's name does not reach the display
    // (designer contract 2.9) - a subscription from an earlier project, say.
    if (projectLoader.isLoaded() && projectLoader.isLocalTopic(topic)) return;
    applyTopicValue(topic, payload);
  });
  mqttClient.connect();
  Serial.printf("[papers3] mqtt %s:%d as %s\n", creds.mqttHost.c_str(), creds.mqttPort,
                mqttClient.getClientId().c_str());
}

void setup()
{
  Serial.begin(115200);
  // Writes wait for a reader for 1 ms at most. The USB serial counts as
  // connected once a host has read from it or written to it - hil-on, hil?,
  // a monitor - and from then on every print with nobody reading waited up
  // to 100 ms for room. The knob logs a lot: a few minutes of HIL and its
  // loop spent so long in prints that WiFi and MQTT starved, until a hard
  // reset (2026-10-14). Not 0: HWCDC::write counts its tries down from the
  // timeout, and from 0 that wraps round - a print then waited until
  // someone read, and the PaperS3's HTTP went silent for minutes. After one
  // 1 ms try without progress it marks the host gone, and later prints pass
  // straight through until a host reads again.
  Serial.setTxTimeoutMs(1);
  delay(200);
  Serial.printf("\n[papers3] %s %s, DDF %s\n", DEVICE_ID, FIRMWARE_VERSION, DDF_HASH);

  if (!setupPanelAndProject()) return;
  setupNetworking();
}

void loop()
{
  M5.update();

  if (restartAtMs && millis() >= restartAtMs) ESP.restart();
  if (!setupModeActive && wifi.tick() && noWifiScreenShown)
  {
    noWifiScreenShown = false;
    panel.forceFullNext();
    renderAndPresent("wifi up");
  }

  // Setup mode owns the device completely: it draws its own screen, it is
  // the only thing on port 80, and nothing else may run - the same shape the
  // 4.3B settled on after the normal loop kept painting the project back
  // over the setup screen.
  if (setupModeActive)
  {
    wifiSetupServer->handleClient();

    // "tap to cancel", which the screen has promised since this board first
    // drew it and which did nothing until 2026-09-14: this branch never read
    // the touch panel at all. A tap anywhere counts, but only one that
    // begins after setup mode did - the finger that just held the corner for
    // ten seconds is still on the glass, and its lift is not a tap.
    static bool setupLiftSeen = false;
    static bool setupTapDown = false;
    bool tapToCancel = false;
    if (M5.Touch.getDetail().isPressed())
    {
      if (setupLiftSeen) setupTapDown = true;
    }
    else
    {
      tapToCancel = setupTapDown;
      setupLiftSeen = true;
    }

    if (wifiSetupServer->tick() || wifiSetupServer->exitRequested() || tapToCancel)
    {
      Serial.printf("[papers3] leaving setup mode%s - restarting\n", tapToCancel ? " (tap to cancel)" : "");
      delay(200);
      ESP.restart();
    }
    return;
  }

  serviceTouch();
  testInterface.handleClient();
  mqttClient.loop();
  // What this display wrote to its local topics, taken in as arrivals.
  if (!localWrites.empty())
  {
    std::vector<std::pair<String, String>> writes;
    writes.swap(localWrites);
    for (const auto &w : writes) applyTopicValue(w.first, w.second);
  }
  subscribeToProjectTopics();
  firmwareUpdater.service();
  deviceRequests.service();
  testInterfaceSwitch.service();
  if (pendingDeploy)
  {
    pendingDeploy = false;
    const String payload = pendingDeployPayload;
    pendingDeployPayload = "";
    deployManager.handleDeployMessage(payload);
  }

  // Not while the hold's countdown is up: a value would paint the project
  // over the box, and the next second's tick would paint the box back - two
  // partials a second on e-ink, for a box nobody should lose sight of. The
  // value is stored already and is drawn when the finger lets go.
  if (renderPending && !holdCountdownShown && !keyboardOpen) renderAndPresent("topic value");

  // The quiet-moment clean-up (decision 6). The canvas still holds the screen
  // the last paint showed, so this is the same picture, painted clean.
  if (panel.cleanupDue())
  {
    panel.forceFullNext();
    renderAndPresent("idle clean-up");
  }

  heartbeat();
  delay(5);
}
