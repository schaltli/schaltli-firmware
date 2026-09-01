// Bring-up sketch for the Waveshare ESP32-S3-Touch-LCD-4.3B.
//
// Milestone 1 of the port: prove the panel alone. No LVGL, no project
// loader, no MQTT, no touch, no IO expander - so that a black screen after
// the first flash can only mean the RGB timings or the pin map, and nothing
// else. Everything above this is added once this is known good.
//
// Deliberately does NOT touch the CH422G expander. Waveshare's own
// 08_DrawColorBar example for this board doesn't either, which is the
// evidence that the backlight (EXIO2/DISP) is on by default after reset.
// Touch (GT911) needs EXIO1 released and therefore waits for milestone 2.
//
// Pin map and RGB timings are Waveshare's own, taken from
// waveshareteam/ESP32-S3-Touch-LCD-4.3B (Apache-2.0),
// examples/Arduino/examples/08_DrawColorBar/waveshare_lcd_port.h, and
// cross-checked line by line against the pin table in the board's wiki.
// Both sources agree; nothing here is inferred from the 1.8" knob port,
// which is the mistake that produced four hardware bugs on the last port.
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <Wire.h>
#include <LittleFS.h>
#include "Ch422g.h"
#include "Gt911.h"
#include "TestProjectJson.h"
#include "FontSample.h"
#include "PanelDisplayAdapter.h"
#include <WiFi.h>
#include "../../NvsConfigStorage.h"
#include "../../WiFiSetupServer.h"
#include "../../MqttClient.h"
#include "../../DeployManager.h"
#include "../../TestInterfaceServer.h"
#include "../../DeviceProtocol.h"
#include "../../ClippedCanvas16.h"
#include "../../project/ProjectLoader.h"
#include "../../project/ColorScreenRenderer.h"

// --- shared rendering stack ----------------------------------------------
// The same ColorScreenRenderer the knob and the M5 Dial run, unchanged. Its
// output goes into an RGB565 canvas; the only board-specific part is where
// that canvas then goes - here a straight blit to the RGB panel, with no
// LVGL in the path. The knob composites through LVGL because it wants
// transitions; that costs a second and third 768KB buffer on this panel and
// is a decision to take with measurements, not before them.
static ProjectLoader projectLoader;
static ClippedCanvas16 *canvas = nullptr;
static ColorScreenRenderer *renderer = nullptr;

static void setupRenderStack();
static void renderAndPresent(const char *reason);
static void benchmarkFill();
static void setupNetworking();

// --- networking ----------------------------------------------------------
// All six of these classes compiled for this board without a single change,
// which is the second time the shared layer has proved to be what it claims.
// What was missing was only the wiring, and that lived in the knob's
// main.cpp rather than anywhere shareable.
static NvsConfigStorage configStorage;
static PanelDisplayAdapter *displayAdapter = nullptr;
static WiFiSetupServer *wifiSetupServer = nullptr;
static MqttClient mqttClient;
static DeployManager deployManager;
static TestInterfaceServer testInterface;
static DeviceProtocol protocol(mqttClient);
static bool topicsSubscribed = false;

// --- shared I2C bus -------------------------------------------------------
// Touch, the CH422G expander and the RTC all sit on the same two lines.
static const int I2C_SDA = 8;
static const int I2C_SCL = 9;
static const int TP_INT = 4;

static Ch422g expander;
static Gt911 touch;

// Result of the most recent backlight-blanking probe, kept so it can be
// reported again later. Catching a transient live meant the observer and
// the reader had to be present in the same few seconds, which failed three
// times running; a result that persists removes the timing from the problem
// altogether.
// In NVS, because everything weaker has already failed here.
//
// Plain RAM was destroyed by the act of reading it - opening the serial port
// resets this board, the trap the knob port documents ("serial monitor
// resets the board, diagnose via GET /api/debug"). RTC memory then also came
// back empty, which is consistent with the RTC watchdog reset that esptool
// and the CDC open both end on: that resets the RTC domain too. Flash
// survives all of it.
//
// Two counters, not one, and that is the point: tapCount rises on every
// press edge whether or not the probe finishes, so "no evidence" can be told
// apart from "the touch never produced an edge". Without that split, four
// attempts in a row said nothing at all.
#include <Preferences.h>
static Preferences prefs;
static uint32_t tapCount = 0;
static uint32_t probeCount = 0;
static uint32_t probeSamples = 0;
static uint32_t probeChanges = 0;
static uint32_t probeFirstHighMs = 0; // 0 = BL never read back high

// --- panel geometry -------------------------------------------------------
static const int16_t PANEL_W = 800;
static const int16_t PANEL_H = 480;

// --- RGB timings (ST7262, 800x480) ---------------------------------------
static const int32_t PCLK_HZ = 16 * 1000 * 1000;
static const uint16_t HSYNC_PULSE_WIDTH = 4;
static const uint16_t HSYNC_BACK_PORCH = 8;
static const uint16_t HSYNC_FRONT_PORCH = 8;
static const uint16_t VSYNC_PULSE_WIDTH = 4;
static const uint16_t VSYNC_BACK_PORCH = 8;
static const uint16_t VSYNC_FRONT_PORCH = 8;

// Bounce buffer, in pixels. The RGB peripheral streams the framebuffer out
// of PSRAM continuously; without a bounce buffer through internal SRAM the
// picture drifts sideways under bus contention. Waveshare uses width * 10.
static const size_t BOUNCE_BUFFER_PX = PANEL_W * 10;

// --- pin map --------------------------------------------------------------
// Waveshare's DATA0..DATA15 is an RGB565 bus in the order B, G, R:
// DATA0-4 = blue, DATA5-10 = green, DATA11-15 = red. Arduino_GFX wants them
// split, hence the regrouping below. The wiki names the same lines B3..B7,
// G2..G7 and R3..R7 - the panel's own bit numbering, same physical pins.
static Arduino_ESP32RGBPanel *panel = new Arduino_ESP32RGBPanel(
    /* de */ 5, /* vsync */ 3, /* hsync */ 46, /* pclk */ 7,
    /* r0..r4 */ 1, 2, 42, 41, 40,
    /* g0..g5 */ 39, 0, 45, 48, 47, 21,
    /* b0..b4 */ 14, 38, 18, 17, 10,
    /* hsync */ 0, HSYNC_FRONT_PORCH, HSYNC_PULSE_WIDTH, HSYNC_BACK_PORCH,
    /* vsync */ 0, VSYNC_FRONT_PORCH, VSYNC_PULSE_WIDTH, VSYNC_BACK_PORCH,
    /* pclk_active_neg */ 1, /* prefer_speed */ PCLK_HZ, /* useBigEndian */ false,
    /* de_idle_high */ 0, /* pclk_idle_high */ 0,
    /* bounce_buffer_size_px */ BOUNCE_BUFFER_PX);

// No panel reset line on this board (LCD_RST is on the expander, and the
// panel comes up without it - Waveshare's example passes -1 as well).
static Arduino_RGB_Display *gfx = new Arduino_RGB_Display(PANEL_W, PANEL_H, panel);

static void drawTestPattern()
{
  // Colour bars first: if the pin grouping were wrong, these come out in
  // the wrong order or with the wrong channel, which is far easier to read
  // off the glass than a wrong-looking photo.
  const uint16_t bars[] = {RGB565_RED, RGB565_GREEN, RGB565_BLUE, RGB565_YELLOW, RGB565_MAGENTA, RGB565_CYAN, RGB565_WHITE, RGB565_BLACK};
  const int16_t barW = PANEL_W / 8;
  for (int i = 0; i < 8; i++)
  {
    gfx->fillRect(i * barW, 0, barW, PANEL_H / 2, bars[i]);
  }

  // A one-pixel frame around the whole panel. If the timings are off, this
  // is where it shows: the frame either does not close or wraps around.
  gfx->drawRect(0, 0, PANEL_W, PANEL_H, RGB565_WHITE);

  gfx->setTextColor(RGB565_WHITE);
  gfx->setTextSize(3);
  gfx->setCursor(20, PANEL_H / 2 + 30);
  gfx->print("ScreenBee 4.3B bring-up");
  gfx->setTextSize(2);
  gfx->setCursor(20, PANEL_H / 2 + 80);
  gfx->printf("%dx%d  pclk %ld MHz", PANEL_W, PANEL_H, (long)(PCLK_HZ / 1000000));

  // Corner markers, so a photograph shows whether the visible area really
  // starts at 0,0 and ends at 799,479 rather than being shifted by a porch.
  gfx->fillRect(0, 0, 20, 20, RGB565_RED);
  gfx->fillRect(PANEL_W - 20, 0, 20, 20, RGB565_GREEN);
  gfx->fillRect(0, PANEL_H - 20, 20, 20, RGB565_BLUE);
  gfx->fillRect(PANEL_W - 20, PANEL_H - 20, 20, 20, RGB565_YELLOW);
}

void setup()
{
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("[4v3b] bring-up start");

  if (!gfx->begin())
  {
    // Nothing to draw on if this fails, so the serial line is the only
    // channel left - which is why this build keeps CDC-on-boot enabled.
    Serial.println("[4v3b] gfx->begin() FAILED");
    return;
  }
  Serial.println("[4v3b] gfx->begin() ok");

  // A framebuffer of 800*480*2 = 768000 bytes has to come out of PSRAM.
  // Report it rather than assume it: this is the number every later
  // decision about transition buffers depends on.
  Serial.printf("[4v3b] PSRAM free: %u of %u bytes\n",
                (unsigned)ESP.getFreePsram(), (unsigned)ESP.getPsramSize());
  Serial.printf("[4v3b] heap free: %u bytes\n", (unsigned)ESP.getFreeHeap());

  gfx->fillScreen(RGB565_BLACK);
  drawTestPattern();
  Serial.println("[4v3b] test pattern drawn");

  // --- milestone 2: expander and touch ------------------------------------
  prefs.begin("4v3b", false);
  tapCount = prefs.getUInt("taps", 0);
  probeCount = prefs.getUInt("probes", 0);
  probeSamples = prefs.getUInt("samples", 0);
  probeChanges = prefs.getUInt("changes", 0);
  probeFirstHighMs = prefs.getUInt("firstHigh", 0);
  Serial.printf("[4v3b] restored from NVS: %lu taps, %lu probes\n",
                (unsigned long)tapCount, (unsigned long)probeCount);

  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(400000);

  // Scan before assuming. The expander answers on four addresses (it has no
  // register pointer - each "register" is its own address), so the scan is
  // also the proof that it is alive, and it shows which address the touch
  // controller picked without having to trust the reset sequence.
  Serial.print("[4v3b] i2c scan:");
  for (uint8_t addr = 0x08; addr < 0x78; addr++)
  {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) Serial.printf(" 0x%02X", addr);
  }
  Serial.println();

  if (!expander.begin())
  {
    Serial.println("[4v3b] CH422G init FAILED");
  }
  else
  {
    Serial.printf("[4v3b] CH422G ok, IO state 0x%02X\n", expander.state());
    // Blink the backlight. This is the only visible proof that the expander
    // really drives EXIO2 - "the backlight is on" was already true in
    // milestone 1 and says nothing about the CH422G.
    //
    // Deliberately slow: the first version used 150ms and the answer that
    // came back was "twice, I think, but I'm not sure". An observation the
    // observer cannot be certain of is not a verification, and the fix is
    // to make the signal unmistakable rather than to argue about it.
    for (int i = 0; i < 3; i++)
    {
      Serial.printf("[4v3b] backlight off (%d/3)\n", i + 1);
      expander.write(Ch422g::EXIO_LCD_BL, false);
      delay(600);
      expander.write(Ch422g::EXIO_LCD_BL, true);
      Serial.printf("[4v3b] backlight on  (%d/3)\n", i + 1);
      delay(400);
    }
    Serial.println("[4v3b] backlight blinked three times, 600ms dark each");
  }

  if (!touch.begin(expander, TP_INT))
  {
    Serial.println("[4v3b] GT911 not found on 0x5D or 0x14");
  }
  else
  {
    char id[5] = {0};
    bool gotId = touch.readProductId(id);
    Serial.printf("[4v3b] GT911 at 0x%02X, product id \"%s\"%s\n",
                  touch.address(), gotId ? id : "?", gotId ? "" : " (read failed)");
  }

  gfx->setTextSize(2);
  gfx->setCursor(20, PANEL_H / 2 + 120);
  gfx->print("Touch: dots follow your finger. Tap = re-render.");

  setupRenderStack();
}

// Renders screen 0 into the canvas and pushes it to the panel, reporting
// both halves separately. The split is the point: rendering is CPU against
// PSRAM, the blit is 768KB across the same bus the panel is already
// streaming from continuously. Which of the two dominates decides whether
// the knob's transition machinery can be ported at all, and that is a
// question for measurement rather than for my earlier guess.
// Which screen is showing. A tap swaps them, so both the box-heavy screen
// and the line-heavy one get timed repeatedly from a warm system instead of
// once at boot.
//
// Screen 1 carries line geometry lifted straight out of a real project:
// twenty lines of 20x260 at stroke 1, which is what one screen of
// test-projects/combined-test-project.zip actually contains. It exists to
// keep an honest number next to the synthetic one - a 720x140 diagonal at
// stroke 4 costs 530ms, twenty real lines cost 61ms together, and only the
// second figure says anything about this board in use.
static int currentScreen = 0;

static U8G2_FOR_ADAFRUIT_GFX sampleU8g2;

static void renderAndPresent(const char *reason)
{
  if (!renderer || !canvas) return;

  // The font sample sits one past the project's own screens in the tap
  // cycle, so it can be reached and left without a reflash while someone
  // walks back and forth judging it from the reading distance.
  const int screenCount = (int)projectLoader.getProject().screens.size();
  if (currentScreen >= screenCount)
  {
    uint32_t s0 = micros();
    drawFontSample(*canvas, sampleU8g2, PANEL_W, PANEL_H);
    uint32_t s1 = micros();
    gfx->draw16bitRGBBitmap(0, 0, canvas->getBuffer(), PANEL_W, PANEL_H);
    Serial.printf("[4v3b] font sample (%s): draw %lums\n", reason,
                  (unsigned long)((s1 - s0) / 1000));
    return;
  }

  uint32_t t0 = micros();
  bool ok = renderer->renderScreen(currentScreen);
  uint32_t t1 = micros();
  gfx->draw16bitRGBBitmap(0, 0, canvas->getBuffer(), PANEL_W, PANEL_H);
  uint32_t t2 = micros();

  Serial.printf("[4v3b] render(screen %d, %s): %s | draw %lums, blit %lums, total %lums\n",
                currentScreen, reason, ok ? "ok" : "FAILED",
                (unsigned long)((t1 - t0) / 1000), (unsigned long)((t2 - t1) / 1000),
                (unsigned long)((t2 - t0) / 1000));
}

static void setupRenderStack()
{
  if (!LittleFS.begin(true))
  {
    Serial.println("[4v3b] LittleFS mount FAILED");
    return;
  }

  // Written out and read back through the real loader rather than parsed
  // from the string directly: that exercises the filesystem path and the
  // public API the deploy flow will use, instead of a shortcut that only
  // this test would ever take.
  File f = LittleFS.open("/render-smoke.json", "w");
  if (!f)
  {
    Serial.println("[4v3b] could not open test project for writing");
    return;
  }
  f.print(TEST_PROJECT_JSON);
  f.close();

  if (!projectLoader.loadProject("/render-smoke.json"))
  {
    Serial.println("[4v3b] loadProject FAILED");
    return;
  }
  Serial.printf("[4v3b] project loaded: \"%s\", %ux%u\n",
                projectLoader.getProject().name.c_str(),
                projectLoader.getProject().screenWidth,
                projectLoader.getProject().screenHeight);

  // 800*480*2 = 768000 bytes. Internal SRAM has nothing like that free, so
  // a non-null buffer here is itself the proof that it landed in PSRAM -
  // but report where, because "it worked" and "it worked for the reason I
  // assumed" are different claims.
  canvas = new ClippedCanvas16(PANEL_W, PANEL_H);
  if (!canvas || !canvas->getBuffer())
  {
    Serial.printf("[4v3b] FATAL: %u-byte canvas allocation failed\n",
                  (unsigned)(PANEL_W * PANEL_H * 2));
    return;
  }
  Serial.printf("[4v3b] canvas at %p (%s), psram free now %u\n",
                canvas->getBuffer(),
                esp_ptr_external_ram(canvas->getBuffer()) ? "PSRAM" : "internal SRAM",
                (unsigned)ESP.getFreePsram());

  renderer = new ColorScreenRenderer(projectLoader, canvas);
  renderAndPresent("first");
  benchmarkFill();
  setupNetworking();
}

// Subscribes to every topic the loaded project names. Done once the project
// and the connection both exist; connect() replays its own subscription list
// on reconnect, so this does not have to run again.
static void subscribeToProjectTopics()
{
  if (topicsSubscribed || !projectLoader.isLoaded() || !mqttClient.isConnected()) return;
  std::vector<String> topics;
  for (const auto &t : projectLoader.getProject().topics) topics.push_back(t.topic);
  if (!topics.empty()) mqttClient.subscribeToKeys(topics);
  topicsSubscribed = true;
  Serial.printf("[4v3b] subscribed to %d project topic(s)\n", (int)topics.size());
}

static void setupNetworking()
{
  if (!canvas) return;

  displayAdapter = new PanelDisplayAdapter(*canvas, *gfx, PANEL_W, PANEL_H);
  configStorage.begin();
  wifiSetupServer = new WiFiSetupServer(*displayAdapter, configStorage);

  WiFiCredentials creds;
  if (!configStorage.loadWiFiCredentials(creds) || creds.ssid.isEmpty())
  {
    // No credentials: the setup AP is the whole user interface at this
    // point, so the panel says so rather than sitting on a test pattern
    // that looks like it is working.
    Serial.println("[4v3b] no WiFi credentials - starting setup AP");
    wifiSetupServer->startAP();
    return;
  }

  Serial.printf("[4v3b] connecting to \"%s\"\n", creds.ssid.c_str());
  displayAdapter->showLines({"WLAN", creds.ssid});
  WiFi.mode(WIFI_STA);
  WiFi.begin(creds.ssid.c_str(), creds.password.c_str());
  for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) delay(250);

  if (WiFi.status() != WL_CONNECTED)
  {
    Serial.println("[4v3b] WiFi connect FAILED");
    displayAdapter->showLines({"WLAN nicht erreichbar", creds.ssid});
    return;
  }
  Serial.printf("[4v3b] WiFi ok, ip %s\n", WiFi.localIP().toString().c_str());

  // The snapshot endpoint reads this canvas directly, which is what gives
  // this board a HIL comparison at all - everything verified so far has been
  // by eye.
  if (testInterface.begin(*canvas)) Serial.println("[4v3b] test interface on :80");

  if (creds.mqttHost.isEmpty())
  {
    Serial.println("[4v3b] no broker configured - skipping MQTT");
    renderAndPresent("after wifi");
    return;
  }

  mqttClient.configure(creds.mqttHost, (uint16_t)creds.mqttPort, creds.mqttUsername, creds.mqttPassword);
  deployManager.setPublishStatusHandler(
      [](const String &id, const String &state, const String &error, int percent) {
        protocol.publishDeployStatus(id, state, error, percent);
      });
  deployManager.setClearTriggerHandler([]() { protocol.clearDeployTrigger(); });

  // Subscribed up front rather than from the connected callback: connect()
  // replays its subscription list on every reconnect, so queueing it once
  // covers every later connection too.
  mqttClient.subscribe(protocol.deviceTopic("deploy"));
  mqttClient.setConnectedCallback([]() {
    protocol.publishHello("http://" + WiFi.localIP().toString() + "/ddf.zip");
    Serial.println("[4v3b] published hello");
  });
  mqttClient.setCallback([](const String &topic, const String &payload) {
    if (topic == protocol.deviceTopic("deploy"))
    {
      deployManager.handleDeployMessage(payload);
      return;
    }
    projectLoader.setTopicValue(topic, payload);
    renderAndPresent("topic");
  });
  mqttClient.connect();
  Serial.printf("[4v3b] mqtt %s:%d as %s\n", creds.mqttHost.c_str(), creds.mqttPort,
                mqttClient.getClientId().c_str());
  renderAndPresent("after mqtt");

}

// Three numbers that between them say where a slow redraw actually goes.
//
// Rendering six objects costs over a second on this panel, while pushing the
// finished 768KB to the display costs 64ms - so it is not the bus, which is
// what I had predicted. The remaining candidates are the canvas fill path
// and the renderer's own per-object work, and they separate cleanly:
//
//   memset      the floor - what the PSRAM will take, no drawing logic
//   fillRect    the same area through GFXcanvas16's public API
//   blit        for scale
//
// If fillRect is near memset, the canvas is fine and the cost is in the
// renderer. If it is far above, the fill path is the problem and a faster
// one is worth writing - which is a much smaller job than reworking a
// renderer whose pixel output is HIL-verified against the designer.
static void benchmarkFill()
{
  if (!canvas || !canvas->getBuffer()) return;

  const size_t bytes = (size_t)PANEL_W * PANEL_H * 2;
  uint16_t *buf = canvas->getBuffer();

  uint32_t t0 = micros();
  memset(buf, 0x00, bytes);
  uint32_t t1 = micros();

  canvas->fillRect(0, 0, PANEL_W, PANEL_H, RGB565_BLUE);
  uint32_t t2 = micros();

  gfx->draw16bitRGBBitmap(0, 0, buf, PANEL_W, PANEL_H);
  uint32_t t3 = micros();

  Serial.printf("[4v3b] fill benchmark over %u bytes: memset %lums, "
                "canvas fillRect %lums, blit %lums\n",
                (unsigned)bytes,
                (unsigned long)((t1 - t0) / 1000),
                (unsigned long)((t2 - t1) / 1000),
                (unsigned long)((t3 - t2) / 1000));

  // Put the real picture back - the benchmark just painted over it.
  renderAndPresent("after benchmark");
}

// A tap blanks the backlight for a full second, then restores it. Self-paced
// and impossible to misread: the observer chooses the moment, and a whole
// second of darkness cannot be confused with a redraw. It also proves touch
// and expander together, in one gesture.
static void blankOnTap()
{
  // Three seconds, and the expander is read back throughout.
  //
  // Reported on real hardware: during the dark phase a single brief flash
  // of light. That is either us letting the line go high for a moment, or
  // it is electrical and nothing to do with the IO state. Reading EXIO back
  // separates the two - a probe that only reported what we *wrote* would
  // confirm any such bug as fine, which is the whole trap.
  Serial.println("[4v3b] tap - backlight off for 3s, sampling EXIO");
  uint32_t t0 = millis();
  expander.write(Ch422g::EXIO_LCD_BL, false);

  // Seeded from the first reading, not from a constant. Seeding with 0xFF
  // guaranteed the first sample counted as a change - the BL bit has just
  // been cleared, so it cannot match - and a counter that always starts at
  // one cannot tell "something happened" from "nothing happened".
  bool haveLast = false;
  uint8_t last = 0;
  uint32_t samples = 0;
  uint32_t changes = 0;
  uint32_t firstHigh = 0;
  while (millis() - t0 < 3000)
  {
    uint8_t state = 0;
    if (expander.read(state))
    {
      samples++;
      bool blHigh = (state & (1u << Ch422g::EXIO_LCD_BL)) != 0;
      if (blHigh && firstHigh == 0) firstHigh = millis() - t0;
      if (!haveLast)
      {
        haveLast = true;
        last = state;
      }
      else if (state != last)
      {
        changes++;
        Serial.printf("[4v3b]   t+%4lums EXIO=0x%02X  (BL bit %s)\n",
                      (unsigned long)(millis() - t0), state, blHigh ? "HIGH" : "low");
        last = state;
      }
    }
    delay(10);
  }

  expander.write(Ch422g::EXIO_LCD_BL, true);

  probeCount++;
  probeSamples = samples;
  probeChanges = changes;
  probeFirstHighMs = firstHigh;
  prefs.putUInt("probes", probeCount);
  prefs.putUInt("samples", probeSamples);
  prefs.putUInt("changes", probeChanges);
  prefs.putUInt("firstHigh", probeFirstHighMs);

  Serial.printf("[4v3b] backlight back on - %lu samples, %lu state changes\n",
                (unsigned long)samples, (unsigned long)changes);
}

void loop()
{
  if (wifiSetupServer) wifiSetupServer->handleClient();
  testInterface.handleClient();
  mqttClient.loop();
  subscribeToProjectTopics();

  static uint32_t last = 0;
  if (millis() - last >= 5000)
  {
    last = millis();
    Serial.printf("[4v3b] alive, uptime %lus | taps %lu, probes %lu",
                  (unsigned long)(millis() / 1000),
                  (unsigned long)tapCount, (unsigned long)probeCount);
    if (probeCount == 0)
    {
      Serial.println(" - no probe recorded yet");
    }
    else
    {
      Serial.printf(" | last probe: %lu samples, %lu changes, BL read back HIGH %s\n",
                    (unsigned long)probeSamples, (unsigned long)probeChanges,
                    probeFirstHighMs
                        ? (String("at t+") + probeFirstHighMs + "ms").c_str()
                        : "never");
    }
  }

  // Draw a dot per contact and print the coordinates. Both matter: the dot
  // shows whether the touch axes line up with the display axes (a mirrored
  // or swapped axis is obvious under a finger and invisible in a log), and
  // the numbers show whether the range really is 0..799 / 0..479 rather
  // than the panel's native orientation.
  Gt911::Point points[Gt911::MAX_POINTS];
  uint8_t n = touch.read(points, Gt911::MAX_POINTS);

  // Edge, not level. A held finger reports the same coordinates about a
  // hundred times a second - measured on this board, identical to the last
  // digit including strength, which is the controller's own filtering and
  // not a stale buffer. Acting on the level would fire this once per poll.
  static bool wasTouched = false;
  bool isTouched = n > 0;
  bool pressEdge = isTouched && !wasTouched;
  wasTouched = isTouched;

  for (uint8_t i = 0; i < n; i++)
  {
    const uint16_t colours[] = {RGB565_WHITE, RGB565_CYAN, RGB565_MAGENTA, RGB565_YELLOW, RGB565_GREEN};
    gfx->fillCircle(points[i].x, points[i].y, 6, colours[i % 5]);
    Serial.printf("[4v3b] touch %u/%u id=%u x=%u y=%u strength=%u\n",
                  i + 1, n, points[i].trackId, points[i].x, points[i].y, points[i].strength);
  }

  if (pressEdge)
  {
    // Counted and persisted *before* the probe runs, so a tap that reaches
    // us is recorded even if everything after it goes wrong.
    tapCount++;
    prefs.putUInt("taps", tapCount);
    Serial.printf("[4v3b] press edge #%lu\n", (unsigned long)tapCount);

    // Swap screens on every tap: repeated timings from a warm system are
    // worth more than a single cold one at boot, and alternating between the
    // box-heavy and the line-heavy screen keeps both numbers current.
    // One past the project's screens is the font sample, so the cycle is
    // screens 0..n-1 then the sample, then back to 0.
    const int screenCount = (int)projectLoader.getProject().screens.size();
    currentScreen = (currentScreen + 1) % (screenCount + 1);
    renderAndPresent("tap");
  }

  delay(n ? 10 : 30);
}
