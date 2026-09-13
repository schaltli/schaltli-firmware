// M5Stack PaperS3 - board bring-up.
//
// Third board in this repo, started 2026-09-13. The decisions it is being
// built to are in the designer repo's docs/2026-09-13-papers3-grilling.md,
// cited by number where they bite.
//
// This file is deliberately the smallest thing that proves the part nobody
// has done here before: getting our own RGB565 canvas onto an e-ink panel
// driven by the ESP32-S3 itself. Everything above that line - project
// loader, renderer, MQTT, deploy, the test interface - is code this repo
// already runs on two boards, and it gets wired in once the panel is known
// to work rather than at the same time, so a first failure has one possible
// cause instead of six.
//
// Why the panel is the unknown: the two existing boards are LCDs that can
// be repainted freely. This one cannot, which is what EpdPanel.h is about.

#include <Arduino.h>
#include "DeviceInfo.h"
#include "EpdPanel.h"
#include "PanelDisplayAdapter.h"
#include "../../ClippedCanvas16.h"

// The panel's own size, read back from M5GFX at boot rather than hardcoded,
// and checked against what the DDF declares. A mismatch here means the
// designer is laying out screens for a panel this device does not have, and
// every pixel comparison afterwards would be measuring that instead of
// whatever it was meant to measure.
static const int16_t DDF_SCREEN_W = 960;
static const int16_t DDF_SCREEN_H = 540;

static EpdPanel panel;
static ClippedCanvas16 *canvas = nullptr;
static PanelDisplayAdapter *display = nullptr;

void setup()
{
  Serial.begin(115200);
  delay(200);
  Serial.printf("\n[papers3] %s %s, DDF %s\n", DEVICE_ID, FIRMWARE_VERSION, DDF_HASH);

  if (!panel.begin())
  {
    Serial.println("[papers3] panel did not come up - nothing else can work, stopping here");
    return;
  }
  Serial.printf("[papers3] panel %dx%d\n", panel.width(), panel.height());

  if (panel.width() != DDF_SCREEN_W || panel.height() != DDF_SCREEN_H)
  {
    // Loud rather than adaptive. Taking the panel's word for it would let
    // the device render happily at a size the designer never laid out for,
    // and the HIL comparison would then fail on dimensions with no hint
    // that the DDF is the thing that is wrong.
    Serial.printf("[papers3] DDF declares %dx%d - fix ddf-source-papers3/device.json\n",
                  DDF_SCREEN_W, DDF_SCREEN_H);
  }

  // ~1MB at 960x540x2. Internal RAM cannot hold that, so this lands in
  // PSRAM through the core's allocator, the same way the 4.3B's 800x480
  // canvas does.
  canvas = new ClippedCanvas16(panel.width(), panel.height());
  if (canvas->getBuffer() == nullptr)
  {
    Serial.println("[papers3] canvas allocation failed - is PSRAM enabled in this build?");
    return;
  }

  display = new PanelDisplayAdapter(*canvas, panel, panel.width(), panel.height());
  display->showLines({
      "ScreenBee",
      String("PaperS3 ") + FIRMWARE_VERSION,
      "",
      "panel up, nothing else wired yet",
  });
  Serial.println("[papers3] boot screen pushed");
}

void loop()
{
  M5.update();
  delay(50);
}
