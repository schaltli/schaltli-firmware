#pragma once
#include <Arduino.h>
#include <WebServer.h>
#include "ClippedCanvas16.h"

// Ported from screenbee-m5dial@d73d0ff on 2026-08-19, deliberately as a
// subset: only the two endpoints that need nothing but a rendered canvas.
//
//   GET /snapshot.bmp - the current framebuffer, 24-bit BMP
//   GET /ddf.zip      - this device's own DDF, compiled in via ddf_zip.h
//
// These two come first in the port on purpose: with them live, every later
// step (project installer, renderer, deploy) is verifiable by pixel-diffing
// a real device against the designer's own render, instead of by looking at
// the screen and forming an opinion. That HIL pipeline is what took the
// e-paper firmware's label rendering to an exact 0-pixel match with the
// designer, finding six real bugs on the way, and it is the reason the
// milestone is sequenced this way.
//
// The endpoints that need the rest of the stack are added as it lands:
// POST /api/screen and POST /api/project (project loader), GET
// /api/topic-values and POST /api/mqtt (MQTT client), GET /recovery-project
// (deploy manager).
class TestInterfaceServer {
public:
  // The canvas is passed to begin() rather than the constructor because it
  // only exists once CanvasPresenter has allocated it, which can fail.
  bool begin(ClippedCanvas16& canvas, uint16_t port = 80);
  void stop();

  // Pump from loop(). Cheap when idle; safe to call before begin().
  void handleClient();

  bool isRunning() const { return serverRunning_; }

private:
  void handleSnapshot();
  void handleDdfZip();

  // Standard 24-bit BITMAPINFOHEADER, no palette - unlike the e-paper
  // firmware's 1-bit-plus-palette header, since this device's DDF declares
  // 24bit colorDepth.
  void generateBMPHeader(uint8_t* header, int width, int height, int dataSize);
  void sendBMP();

  WebServer* webServer_ = nullptr;
  ClippedCanvas16* canvas_ = nullptr;
  bool serverRunning_ = false;
};
