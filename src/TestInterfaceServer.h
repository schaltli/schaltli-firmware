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
//   POST /api/forget-wifi - clear stored credentials and reboot into the
//                        setup portal. The M5 Dial reaches setup mode with a
//                        long button press; this board has no button, so
//                        without this there is no way back into it short of
//                        a reflash - which also matters for testing the
//                        portal at all.
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

  // Reads back what the device currently holds for one topic. An
  // orchestrator publishes a value over MQTT and then polls this until the
  // device reports it back, instead of sleeping a fixed amount and hoping -
  // the wrong render captured because a snapshot was taken a few
  // milliseconds early is indistinguishable from a real rendering bug.
  using TopicValueHandler = std::function<String(const String& topic)>;

  // Fires a named input's configured action as if it had come from the
  // hardware. Splits a question that is otherwise untestable in two: whether
  // the *gesture and encoder decoding* work needs a human's hands, but
  // whether the *action bound to that input* resolves and runs correctly is
  // ordinary logic, and this makes that half automatable. Without it every
  // change to action dispatch would need someone standing at the device.
  // `deferRender` asks for the input to be dispatched exactly as the
  // hardware would deliver it, rate limit and all. It defaults to false:
  // a caller over HTTP dispatches one input and snapshots as soon as this
  // returns, so by default the resulting frame has to be on the panel by
  // then, whatever navFrameMs happens to be set to. Only a test that is
  // deliberately bursting - measuring the rate limit itself - passes true.
  using InputHandler = std::function<void(const String& buttonId, bool deferRender)>;

  // One synthetic touch sample, fed to the firmware as if the panel had
  // reported it. Exists because a gesture is not otherwise reachable from a
  // test: POST /api/input dispatches the *action* a swipe is bound to, which
  // says nothing about whether a drag decodes into that swipe, follows the
  // finger, or settles where it should. Posting a series of these drives the
  // real detector, the real transition and the real settle.
  using TouchHandler = std::function<void(int16_t x, int16_t y, bool down)>;

  // A copy of every pixel that has actually been pushed to the panel, or
  // null if the firmware is not keeping one.
  //
  // /snapshot.bmp serves the canvas, which is what the renderer *drew*. The
  // panel has no framebuffer anyone here can read, and it keeps whatever was
  // last written to it, so the two can disagree - and twice now a fault has
  // been invisible from the canvas side while being the only thing on the
  // glass: a screen frozen part-way through a swipe, with the canvas, the
  // object positions and the watchdog all reporting perfect health. This is
  // the readback that ends that argument.
  using PanelShadowHandler = std::function<const uint16_t*()>;

  // Serves whatever JSON the firmware wants to expose for bring-up
  // diagnosis at GET /api/debug. Exists because serial monitoring is not a
  // neutral observer on this board: the S3's native USB interprets DTR/RTS
  // line changes as chip-reset requests (rst:0x15, USB_UART_CHIP_RESET), so
  // watching the device over COM can itself reboot it - HTTP cannot.
  //
  // The argument is the request's "set" query parameter ("key=value", empty
  // when absent), so diagnosis parameters can be adjusted per request
  // without a reflash - what to do with it is the handler's business.
  using DebugHandler = std::function<String(const String& setArg)>;

  // The canvas is passed to begin() rather than the constructor because it
  // only exists once CanvasPresenter has allocated it, which can fail.
  bool begin(ClippedCanvas16& canvas, uint16_t port = 80);

  void setScreenSwitchHandler(ScreenSwitchHandler handler) { screenSwitchHandler_ = handler; }
  void setTopicValueHandler(TopicValueHandler handler) { topicValueHandler_ = handler; }
  void setInputHandler(InputHandler handler) { inputHandler_ = handler; }
  void setTouchHandler(TouchHandler handler) { touchHandler_ = handler; }
  void setPanelShadowHandler(PanelShadowHandler handler) { panelShadowHandler_ = handler; }

  // Called after a device setting is saved, so the change takes effect
  // without a restart - someone who just set a timeout wants to see it
  // work, and a setting that needs a reboot invites power-cycling a device
  // that is running fine.
  using SettingsChangedHandler = std::function<void()>;
  void setSettingsChangedHandler(SettingsChangedHandler handler) { settingsChangedHandler_ = handler; }

  // The installed project's screens as a JSON array of {"id","name"}, for
  // the device settings page's "return to" dropdown. Same handler, same
  // shape and set from the same place as WiFiSetupServer's - the two serve
  // the identical form and must be able to fill it identically.
  using ScreenListHandler = std::function<String()>;
  void setScreenListHandler(ScreenListHandler handler) { screenListHandler_ = handler; }
  void setDebugHandler(DebugHandler handler) { debugHandler_ = handler; }

  // Signalmodus: das Panel zeigt seinen eigenen Empfangspegel gross auf dem
  // Display, statt ihn nur ueber /api/debug zu melden.
  //
  // Warum auf dem Geraet und nicht am Rechner (2026-08-26): der Pegel wird
  // gesucht, indem man das Panel in die Hand nimmt, das Gehaeuse oeffnet und
  // durchs Fahrzeug laeuft. Wer dabei auf einen Laptop schauen muss, misst
  // nicht das, was er gerade tut. Und ein Wert, der ueber MQTT oder HTTP
  // kaeme, friert genau dann ein, wenn es interessant wird - bei schlechtem
  // Signal. Lokal gerendert bleibt er ablesbar, auch wenn das Netz weg ist.
  //
  // Der Parameter ist -1 = umschalten, 0 = aus, 1 = an. Zurueck kommt der
  // Zustand danach.
  using SignalModeHandler = std::function<bool(int wunsch)>;
  void setSignalModeHandler(SignalModeHandler handler) { signalModeHandler_ = handler; }
  void stop();

  // Pump from loop(). Cheap when idle; safe to call before begin().
  void handleClient();

  bool isRunning() const { return serverRunning_; }

private:
  void handleSnapshot();
  void handleDdfZip();
  void handleScreenSwitch();
  void handleSignalMode();
  void handleMqttConfigure();
  void handleForgetWiFi();
  void handleGetTopicValues();
  void handleInput();
  void handleTouch();
  void handleRecoveryProject();
  void handleFilesystemInfo();
  void handleSettingsPage();
  void handleSettingsGet();
  void handleSettingsSave();
  void handlePanel();
  void sendBMP(const uint16_t* source);
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
  SignalModeHandler signalModeHandler_;
  TopicValueHandler topicValueHandler_;
  InputHandler inputHandler_;
  TouchHandler touchHandler_;
  PanelShadowHandler panelShadowHandler_;
  SettingsChangedHandler settingsChangedHandler_;
  ScreenListHandler screenListHandler_;
  DebugHandler debugHandler_;
  File uploadFile_;
  // Why the last install refused, kept so it can be read back over HTTP.
  // The upload reply is sent before the install even runs ("Processing
  // upload..."), so a refusal used to reach nobody but the serial console -
  // and attaching that resets this board, which is how a full filesystem
  // spent two upload attempts looking like a device that simply ignored
  // them.
  String lastInstallError_;
};
