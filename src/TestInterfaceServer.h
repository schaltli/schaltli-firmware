#pragma once
#include <Arduino.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <functional>
#include "ClippedCanvas16.h"

// Ported from screenbee-m5dial@d73d0ff on 2026-08-19, deliberately as a
// subset: only the two endpoints that need nothing but a rendered canvas.
//
//   GET  /snapshot.bmp - the current framebuffer, 24-bit BMP
//   GET  /ddf.zip      - this device's own DDF, compiled in via ddf_zip.h
//   POST /api/project  - upload+install a project zip, then reboot into it
//   POST /api/screen   - force a full render of one screen index
//   POST /api/mqtt     - store broker settings in NVS (applied on reboot)
//
// These two come first in the port on purpose: with them live, every later
// step (project installer, renderer, deploy) is verifiable by pixel-diffing
// a real device against the designer's own render, instead of by looking at
// the screen and forming an opinion. That HIL pipeline is what took the
// e-paper firmware's label rendering to an exact 0-pixel match with the
// designer, finding six real bugs on the way, and it is the reason the
// milestone is sequenced this way.
//
// Still to come as the rest of the stack lands: GET /api/topic-values and
// POST /api/mqtt (MQTT client), GET /recovery-project (deploy manager).
class TestInterfaceServer {
public:
  // Renders the given screen index in full and blits it. Returns false for
  // an out-of-range index or a failed render. Always a *full* render, never
  // anything depending on what was on screen before, so repeated snapshots
  // of the same index are byte-reproducible - which is the entire basis of
  // pixel-diffing against the designer.
  using ScreenSwitchHandler = std::function<bool(int index)>;

  // The canvas is passed to begin() rather than the constructor because it
  // only exists once CanvasPresenter has allocated it, which can fail.
  bool begin(ClippedCanvas16& canvas, uint16_t port = 80);

  void setScreenSwitchHandler(ScreenSwitchHandler handler) { screenSwitchHandler_ = handler; }
  void stop();

  // Pump from loop(). Cheap when idle; safe to call before begin().
  void handleClient();

  bool isRunning() const { return serverRunning_; }

private:
  void handleSnapshot();
  void handleDdfZip();
  void handleScreenSwitch();
  void handleMqttConfigure();
  void handleProjectUploadChunk();
  void handleProjectUploadComplete();
  bool validateAndInstallZip();
  void sendJSONResponse(bool success, const String& message);

  // Standard 24-bit BITMAPINFOHEADER, no palette - unlike the e-paper
  // firmware's 1-bit-plus-palette header, since this device's DDF declares
  // 24bit colorDepth.
  void generateBMPHeader(uint8_t* header, int width, int height, int dataSize);
  void sendBMP();

  WebServer* webServer_ = nullptr;
  ClippedCanvas16* canvas_ = nullptr;
  bool serverRunning_ = false;
  ScreenSwitchHandler screenSwitchHandler_;
  File uploadFile_;
};
