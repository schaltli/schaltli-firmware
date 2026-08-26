#include "TestInterfaceServer.h"
#include "ddf_zip.h"
#include "DeviceInfo.h"
#include "DeviceSettingsEndpoint.h"
#include "DeviceSettingsHTML.h"
#include "project/ProjectInstaller.h"
#include <ArduinoJson.h>
#include <Preferences.h>

bool TestInterfaceServer::begin(ClippedCanvas16& canvas, uint16_t port) {
  if (serverRunning_) return true;

  canvas_ = &canvas;
  webServer_ = new WebServer(port);
  webServer_->on("/snapshot.bmp", HTTP_GET, [this]() { this->handleSnapshot(); });
  webServer_->on("/panel.bmp", HTTP_GET, [this]() { this->handlePanel(); });
  webServer_->on("/ddf.zip", HTTP_GET, [this]() { this->handleDdfZip(); });
  webServer_->on("/api/screen", HTTP_POST, [this]() { this->handleScreenSwitch(); });
  webServer_->on("/api/mqtt", HTTP_POST, [this]() { this->handleMqttConfigure(); });
  webServer_->on("/api/forget-wifi", HTTP_POST, [this]() { this->handleForgetWiFi(); });
  webServer_->on("/api/topic-values", HTTP_GET, [this]() { this->handleGetTopicValues(); });
  webServer_->on("/api/input", HTTP_POST, [this]() { this->handleInput(); });
  webServer_->on("/api/touch", HTTP_POST, [this]() { this->handleTouch(); });
  webServer_->on("/recovery-project", HTTP_GET, [this]() { this->handleRecoveryProject(); });
  // What the filesystem is holding, and why the last install refused. A
  // project install writes the upload to LittleFS before unpacking it, so a
  // filesystem with no room refuses installs while the device otherwise
  // behaves perfectly - it renders, answers, reports healthy. Without this
  // the only way to see that was a serial monitor, whose attach resets the
  // board.
  webServer_->on("/api/fs", HTTP_GET, [this]() { this->handleFilesystemInfo(); });
  // Device settings, served in normal operation rather than only from the
  // setup AP. Needing to put a device into setup mode to change how long
  // its screen stays lit would mean taking it off the network to adjust a
  // display preference - the same shape of problem as having to forget the
  // WiFi to reach the config page.
  webServer_->on("/settings", HTTP_GET, [this]() { this->handleSettingsPage(); });
  webServer_->on("/api/device-settings", HTTP_GET, [this]() { this->handleSettingsGet(); });
  webServer_->on("/api/device-settings", HTTP_POST, [this]() { this->handleSettingsSave(); });
  // Signalmodus an/aus. Absichtlich auch per GET: umgeschaltet wird das vom
  // Handy im selben Netz, waehrend man das Panel in der Hand haelt - und
  // dort tippt man eine URL in den Browser, keinen POST.
  webServer_->on("/api/signal", HTTP_GET, [this]() { this->handleSignalMode(); });
  webServer_->on("/api/signal", HTTP_POST, [this]() { this->handleSignalMode(); });
  webServer_->on("/api/debug", HTTP_GET, [this]() {
    webServer_->send(200, "application/json",
                     debugHandler_ ? debugHandler_(webServer_->arg("set")) : String("{}"));
  });
  webServer_->on(
      "/api/project", HTTP_POST, [this]() { this->handleProjectUploadComplete(); },
      [this]() { this->handleProjectUploadChunk(); });
  webServer_->begin();
  serverRunning_ = true;

  Serial.printf("[TestInterface] listening on port %u: /snapshot.bmp, /ddf.zip, /api/screen, /api/project\n", port);
  return true;
}

void TestInterfaceServer::sendJSONResponse(bool success, const String& message) {
  JsonDocument doc;
  doc["success"] = success;
  doc["message"] = message;
  String output;
  serializeJson(doc, output);
  webServer_->send(success ? 200 : 400, "application/json", output);
}

void TestInterfaceServer::handleSignalMode() {
  if (!signalModeHandler_) {
    sendJSONResponse(false, "Signal mode is not available on this device");
    return;
  }

  // Ohne Parameter umschalten - das ist der Aufruf, den man vom Handy aus
  // macht, und zweimal dieselbe URL soll an und wieder aus bedeuten.
  int wunsch = -1;
  if (webServer_->hasArg("on")) {
    const String v = webServer_->arg("on");
    wunsch = (v == "1" || v == "true" || v == "on") ? 1 : 0;
  }

  const bool aktiv = signalModeHandler_(wunsch);

  JsonDocument doc;
  doc["success"] = true;
  doc["signalMode"] = aktiv;
  String output;
  serializeJson(doc, output);
  webServer_->send(200, "application/json", output);
}

void TestInterfaceServer::handleScreenSwitch() {
  if (!webServer_->hasArg("index")) {
    sendJSONResponse(false, "Missing 'index' parameter");
    return;
  }
  if (!screenSwitchHandler_) {
    sendJSONResponse(false, "Screen switching is not available on this device");
    return;
  }

  int index = webServer_->arg("index").toInt();
  if (!screenSwitchHandler_(index)) {
    sendJSONResponse(false, "Invalid screen index or render failed: " + String(index));
    return;
  }

  JsonDocument doc;
  doc["success"] = true;
  doc["screenIndex"] = index;
  String output;
  serializeJson(doc, output);
  webServer_->send(200, "application/json", output);
}

// Writes the same NVS namespace this board's own setup UI uses, so a HIL
// orchestrator can point the device at a test broker without a human
// touching the screen. Applied on the next boot, matching how the settings
// are read (setupMqtt() in main.cpp reads them once at startup).
void TestInterfaceServer::handleMqttConfigure() {
  if (!webServer_->hasArg("host") || !webServer_->hasArg("port")) {
    sendJSONResponse(false, "Missing MQTT host or port");
    return;
  }
  int port = webServer_->arg("port").toInt();
  if (port <= 0 || port > 65535) {
    sendJSONResponse(false, "Invalid port number");
    return;
  }

  Preferences prefs;
  prefs.begin("mqtt", false);
  prefs.putString("protocol", webServer_->hasArg("protocol") ? webServer_->arg("protocol") : String("mqtt"));
  prefs.putString("host", webServer_->arg("host"));
  prefs.putString("port", String(port));
  prefs.putString("user", webServer_->arg("username"));
  prefs.putString("pass", webServer_->arg("password"));
  prefs.end();

  sendJSONResponse(true, "MQTT configuration saved - reboot to apply");
}

// POST /api/input  id=swipe-up | button-0 | button-1 | ...
//
// Runs whatever action the current screen binds to that input, exactly as
// the real gesture or knob would. Deliberately does NOT simulate the gesture
// itself - swipe decoding and quadrature decoding still need a human's hands
// to verify - but everything downstream of "an input fired" becomes testable
// without one, which is the part that changes most often.
// GET /recovery-project - the last successfully-verified deploy, which
// DeployManager promoted to RECOVERY_PROJECT_PATH and has kept unchanged
// since (it survives a failed extraction; see DeployManager.cpp).
//
// Ported from screenbee-m5dial on 2026-08-23, having been missed when this
// firmware was written: DeployManager saved the copy and DeviceInfo.h
// described the endpoint that reads it back, but the handler itself never
// existed here. The designer's "Recover project from device" therefore got
// a 404 from every Waveshare and reported that the device had never had a
// project deployed - while the device was displaying that very project.
// Storing something recoverable and offering no way to fetch it is worse
// than not storing it, because it reads as a deliberate answer.
// A page, not only an endpoint: these are the settings a person changes by
// hand, and "curl the device" is no answer for someone who wants their
// screen to stay lit a little longer.
void TestInterfaceServer::handleSettingsPage() {
  // The Device tab of the setup portal's config page, in a standalone
  // shell. Same markup, same script, same endpoint - only the page around
  // it differs, because the two servers own port 80 at different times and
  // a setting reachable from only one of them is a setting you have to put
  // the device into the wrong mode to change. See DeviceSettingsHTML.h.
  //
  // The styling repeats the config page's class names rather than its own
  // former dark theme, so the form looks the same wherever it is met.
  String page = F("<!DOCTYPE html><html><head><meta charset=\"UTF-8\">"
                  "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
                  "<title>ScreenBee Device Settings</title><style>"
                  "*{margin:0;padding:0;box-sizing:border-box}"
                  "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Arial,sans-serif;"
                  "background:linear-gradient(135deg,#667eea 0%,#764ba2 100%);min-height:100vh;padding:20px}"
                  ".container{max-width:600px;margin:0 auto;background:white;border-radius:16px;"
                  "box-shadow:0 20px 60px rgba(0,0,0,0.3);overflow:hidden}"
                  ".header{background:linear-gradient(135deg,#667eea 0%,#764ba2 100%);color:white;"
                  "padding:30px;text-align:center}"
                  ".header h1{font-size:28px;margin-bottom:5px}"
                  ".header p{opacity:0.9;font-size:14px}"
                  ".body{padding:30px}"
                  ".form-group{margin-bottom:20px}"
                  "label{display:block;margin-bottom:8px;font-weight:600;color:#333}"
                  "input,select{width:100%;padding:12px;border:2px solid #e0e0e0;border-radius:8px;"
                  "font-size:15px;transition:border-color 0.3s}"
                  "input:focus,select:focus{outline:none;border-color:#667eea}"
                  ".hint{opacity:.65;font-size:13px;margin-top:6px;line-height:1.45}"
                  ".btn{width:100%;padding:15px;background:linear-gradient(135deg,#667eea 0%,#764ba2 100%);"
                  "color:white;border:none;border-radius:8px;font-size:16px;font-weight:600;cursor:pointer}"
                  ".message{padding:15px;border-radius:8px;margin-bottom:20px;display:none}"
                  ".message.show{display:block}"
                  ".message.success{background:#e8f5e9;color:#2e7d32;border-left:4px solid #4caf50}"
                  ".message.error{background:#ffebee;color:#c62828;border-left:4px solid #f44336}"
                  "</style></head><body><div class=\"container\">"
                  "<div class=\"header\"><h1>Device Settings</h1><p>");
  page += DEVICE_ID;
  page += F("</p></div><div class=\"body\">");
  page += DEVICE_SETTINGS_TAB_HTML;
  page += F("</div></div><script>");
  page += DEVICE_SETTINGS_JS;
  page += F("</script></body></html>");
  webServer_->send(200, "text/html", page);
}

void TestInterfaceServer::handleSettingsGet() {
  const String screens = screenListHandler_ ? screenListHandler_() : String("[]");
  webServer_->send(200, "application/json", DeviceSettingsEndpoint::currentJson(screens));
}

void TestInterfaceServer::handleSettingsSave() {
  String message;
  if (!DeviceSettingsEndpoint::save(*webServer_, message)) {
    sendJSONResponse(false, message);
    return;
  }
  // Applied live rather than at the next boot - see setSettingsChangedHandler.
  if (settingsChangedHandler_) settingsChangedHandler_();
  sendJSONResponse(true, message);
}

void TestInterfaceServer::handleRecoveryProject() {
  if (!LittleFS.exists(RECOVERY_PROJECT_PATH)) {
    webServer_->send(404, "text/plain", "No recovery copy available on this device yet");
    return;
  }

  File f = LittleFS.open(RECOVERY_PROJECT_PATH, "r");
  if (!f) {
    webServer_->send(500, "text/plain", "Failed to open the recovery copy");
    return;
  }

  webServer_->setContentLength(f.size());
  webServer_->sendHeader("Content-Disposition", "attachment; filename=\"recovered_project.zip\"");
  webServer_->send(200, "application/zip", "");

  uint8_t buffer[1024];
  while (f.available()) {
    size_t n = f.read(buffer, sizeof(buffer));
    if (n == 0) break;
    webServer_->client().write(buffer, n);
  }
  f.close();
}

void TestInterfaceServer::handleTouch() {
  if (!touchHandler_) {
    sendJSONResponse(false, "Touch injection is not available on this device");
    return;
  }
  if (!webServer_->hasArg("x") || !webServer_->hasArg("y")) {
    sendJSONResponse(false, "Missing 'x'/'y' parameter");
    return;
  }
  const int16_t x = (int16_t)webServer_->arg("x").toInt();
  const int16_t y = (int16_t)webServer_->arg("y").toInt();
  // Absent means down: a drag is mostly down-samples, and the lift is the
  // one call that has to say so.
  const bool down = webServer_->arg("down") != "0";
  touchHandler_(x, y, down);
  sendJSONResponse(true, String("Touch ") + (down ? "down" : "up") + " at " + x + "," + y);
}

void TestInterfaceServer::handleInput() {
  if (!webServer_->hasArg("id")) {
    sendJSONResponse(false, "Missing 'id' parameter");
    return;
  }
  if (!inputHandler_) {
    sendJSONResponse(false, "Input dispatch is not available on this device");
    return;
  }
  String id = webServer_->arg("id");
  // defer=1 reproduces a physical burst: the frame may be withheld by the
  // navigation rate limit instead of being forced out before the ack.
  const bool deferRender = webServer_->arg("defer") == "1";
  inputHandler_(id, deferRender);
  sendJSONResponse(true, "Dispatched " + id);
}

// GET /api/topic-values?topics=a,b,c -> {"a": "...", "b": "...", ...}
//
// Exists so an orchestrator can wait for a published MQTT value to actually
// land before snapshotting, rather than sleeping and hoping. A snapshot
// taken a few milliseconds early captures the previous value and looks
// exactly like a rendering bug, which is an expensive thing to chase.
void TestInterfaceServer::handleGetTopicValues() {
  if (!topicValueHandler_) {
    sendJSONResponse(false, "Topic value lookup is not available on this device");
    return;
  }

  String topicsParam = webServer_->arg("topics");
  JsonDocument doc;
  int start = 0;
  while (start <= (int)topicsParam.length()) {
    int comma = topicsParam.indexOf(',', start);
    String topic = (comma == -1) ? topicsParam.substring(start) : topicsParam.substring(start, comma);
    topic.trim();
    if (!topic.isEmpty()) doc[topic] = topicValueHandler_(topic);
    if (comma == -1) break;
    start = comma + 1;
  }

  String output;
  serializeJson(doc, output);
  webServer_->send(200, "application/json", output);
}

// Clears the stored network and reboots, which lands the device in the AP
// setup portal. Deliberately leaves the MQTT namespace and the installed
// project alone - this is "put me back in setup", not a factory wipe, and
// re-entering broker settings after every WiFi change would be needless.
//
// The M5 Dial gets here with a long button press. This board has no button,
// so without an endpoint there is no route back into setup short of a
// reflash - and no way to exercise the portal in a test at all.
void TestInterfaceServer::handleForgetWiFi() {
  Preferences prefs;
  if (prefs.begin("wifi", false)) {
    prefs.clear();
    prefs.end();
  }
  sendJSONResponse(true, "WiFi credentials cleared - rebooting into setup mode");
  // Let the response actually reach the caller before the reset.
  delay(500);
  ESP.restart();
}

void TestInterfaceServer::handleProjectUploadChunk() {
  HTTPUpload& upload = webServer_->upload();

  if (upload.status == UPLOAD_FILE_START) {
    // A previous interrupted attempt may have left this open - LittleFS
    // refuses to remove/reopen an already-open path, so close first.
    if (uploadFile_) uploadFile_.close();
    if (LittleFS.exists("/temp_upload.zip")) LittleFS.remove("/temp_upload.zip");
    uploadFile_ = LittleFS.open("/temp_upload.zip", FILE_WRITE);
    if (!uploadFile_) Serial.println("[TestInterface] temp_upload.zip open failed");
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (uploadFile_) uploadFile_.write(upload.buf, upload.currentSize);
  } else if (upload.status == UPLOAD_FILE_END) {
    if (!uploadFile_) return;
    uploadFile_.close();

    if (!LittleFS.exists("/temp_upload.zip")) return;
    File checkFile = LittleFS.open("/temp_upload.zip", "r");
    if (!checkFile) return;
    size_t fileSize = checkFile.size();
    checkFile.close();
    if (fileSize < 100) {
      // Almost always a short *write*, not a short upload: LittleFS fails
      // the write when it is full and what lands on disk is a stub. Say so
      // with the numbers - a bare "zip too small" sent two perfectly good
      // uploads chasing the wrong problem.
      lastInstallError_ = "upload landed as only " + String(fileSize) +
                          " bytes - filesystem has " +
                          String(LittleFS.totalBytes() - LittleFS.usedBytes()) +
                          " of " + String(LittleFS.totalBytes()) + " bytes free";
      Serial.printf("[TestInterface] %s\n", lastInstallError_.c_str());
      LittleFS.remove("/temp_upload.zip");
      return;
    }

    bool ok = validateAndInstallZip();
    LittleFS.remove("/temp_upload.zip");

    if (ok) {
      lastInstallError_ = "";
      // Reboot to pick the newly-installed /PROJECT/project.json up fresh,
      // rather than tearing down and rebuilding live render state
      // mid-request. A HIL orchestrator polling for the device to come back
      // already expects this.
      delay(2000);
      ESP.restart();
    } else {
      Serial.println("[TestInterface] install failed");
    }
  }
}

void TestInterfaceServer::handleProjectUploadComplete() {
  sendJSONResponse(true, "Processing upload...");
}

// Walks one directory recursively, appending {"name","size"} objects.
static void appendDirJson(const char* path, String& out, bool& first) {
  File dir = LittleFS.open(path);
  if (!dir || !dir.isDirectory()) return;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    String full = String(path);
    if (!full.endsWith("/")) full += "/";
    full += f.name();
    if (f.isDirectory()) {
      appendDirJson(full.c_str(), out, first);
    } else {
      if (!first) out += ",";
      first = false;
      out += "{\"name\":\"" + full + "\",\"size\":" + String(f.size()) + "}";
    }
  }
}

void TestInterfaceServer::handleFilesystemInfo() {
  String json = "{";
  json += "\"totalBytes\":" + String(LittleFS.totalBytes());
  json += ",\"usedBytes\":" + String(LittleFS.usedBytes());
  json += ",\"freeBytes\":" + String(LittleFS.totalBytes() - LittleFS.usedBytes());
  json += ",\"lastInstallError\":\"" + lastInstallError_ + "\"";
  json += ",\"files\":[";
  bool first = true;
  appendDirJson("/", json, first);
  json += "]}";
  webServer_->send(200, "application/json", json);
}


bool TestInterfaceServer::validateAndInstallZip() {
  // Generation first, before anything else in the file is trusted - a shape
  // this firmware cannot parse says nothing reliable about its own
  // deviceId either. Absent reads as generation 1, the implicit value every
  // artifact written before the field existed carries, so older exports
  // still install. Only *major* gates: a newer minor is additive by
  // definition and is handled by the renderer's existing "unknown type ->
  // skip and log" and "missing field -> default" conventions.
  long uploadedMajor = ProjectInstaller::peekProjectSystemGenerationMajor("/temp_upload.zip");
  if (uploadedMajor > SYSTEM_GENERATION_MAJOR) {
    lastInstallError_ = "project is system generation " + String(uploadedMajor) +
                        ".x, this firmware understands " + String(SYSTEM_GENERATION_MAJOR) +
                        "." + String(SYSTEM_GENERATION_MINOR);
    Serial.printf("[TestInterface] %s - refusing\n", lastInstallError_.c_str());
    return false;
  }

  // Empty means the zip's project.json has no deviceId at all (older export,
  // or a non-project zip) - not proof it is wrong, so it is not rejected on
  // that basis alone.
  String uploadedDeviceId = ProjectInstaller::peekProjectDeviceId("/temp_upload.zip");
  if (!uploadedDeviceId.isEmpty() && uploadedDeviceId != DEVICE_ID) {
    lastInstallError_ = "deviceId mismatch: zip says \"" + uploadedDeviceId +
                        "\", this is \"" + String(DEVICE_ID) + "\"";
    Serial.printf("[TestInterface] %s\n", lastInstallError_.c_str());
    return false;
  }

  String error;
  if (!ProjectInstaller::installProjectZipFromFile("/temp_upload.zip", error)) {
    lastInstallError_ = error;
    Serial.print("[TestInterface] install error: ");
    Serial.println(error);
    return false;
  }
  return true;
}

void TestInterfaceServer::stop() {
  if (!serverRunning_) return;
  webServer_->stop();
  serverRunning_ = false;
}

void TestInterfaceServer::handleClient() {
  if (serverRunning_) {
    webServer_->handleClient();
  }
}

void TestInterfaceServer::handleSnapshot() {
  sendBMP(canvas_->getBuffer());
}

void TestInterfaceServer::handlePanel() {
  const uint16_t* shadow = panelShadowHandler_ ? panelShadowHandler_() : nullptr;
  if (shadow == nullptr) {
    sendJSONResponse(false, "No panel shadow on this build");
    return;
  }
  sendBMP(shadow);
}

void TestInterfaceServer::handleDdfZip() {
  // send_P(), not send() - the zip's raw bytes contain embedded 0x00s
  // throughout, and every send() overload takes a null-terminated
  // String/const char* with no explicit length, which would silently
  // truncate at the first one. Only send_P()'s 4-arg overload accepts an
  // explicit content length.
  webServer_->send_P(200, "application/zip", (const char*)DDF_ZIP, sizeof(DDF_ZIP));
}

// RGB565 -> 8-bit-per-channel uses bit replication (top bits repeated into
// the low bits, e.g. r8 = (r5<<3)|(r5>>2)) rather than naive left-shift-only
// scaling, so 0x1F (max 5-bit) maps to 0xFF (max 8-bit) instead of 0xF8 -
// the standard, more accurate expansion. Getting this wrong would put a
// constant few-LSB bias into every HIL pixel comparison.
void TestInterfaceServer::generateBMPHeader(uint8_t* header, int width, int height, int dataSize) {
  int fileSize = 54 + dataSize;

  header[0] = 'B';
  header[1] = 'M';
  header[2] = fileSize & 0xFF;
  header[3] = (fileSize >> 8) & 0xFF;
  header[4] = (fileSize >> 16) & 0xFF;
  header[5] = (fileSize >> 24) & 0xFF;
  header[6] = 0;
  header[7] = 0;
  header[8] = 0;
  header[9] = 0;
  header[10] = 54; // Offset to pixel data (14 + 40, no palette)
  header[11] = 0;
  header[12] = 0;
  header[13] = 0;

  header[14] = 40; // DIB header size
  header[15] = 0;
  header[16] = 0;
  header[17] = 0;
  header[18] = width & 0xFF;
  header[19] = (width >> 8) & 0xFF;
  header[20] = (width >> 16) & 0xFF;
  header[21] = (width >> 24) & 0xFF;
  header[22] = height & 0xFF;
  header[23] = (height >> 8) & 0xFF;
  header[24] = (height >> 16) & 0xFF;
  header[25] = (height >> 24) & 0xFF;
  header[26] = 1; // Planes
  header[27] = 0;
  header[28] = 24; // Bits per pixel
  header[29] = 0;
  header[30] = 0; // Compression (none)
  header[31] = 0;
  header[32] = 0;
  header[33] = 0;
  header[34] = dataSize & 0xFF;
  header[35] = (dataSize >> 8) & 0xFF;
  header[36] = (dataSize >> 16) & 0xFF;
  header[37] = (dataSize >> 24) & 0xFF;
  header[38] = 0x13; // X pixels per meter (72 DPI)
  header[39] = 0x0B;
  header[40] = 0;
  header[41] = 0;
  header[42] = 0x13; // Y pixels per meter
  header[43] = 0x0B;
  header[44] = 0;
  header[45] = 0;
  header[46] = 0; // Colors used (n/a, no palette)
  header[47] = 0;
  header[48] = 0;
  header[49] = 0;
  header[50] = 0; // Important colors
  header[51] = 0;
  header[52] = 0;
  header[53] = 0;
}

void TestInterfaceServer::sendBMP(const uint16_t* source) {
  int width = canvas_->width();
  int height = canvas_->height();
  int rowBytes = width * 3;
  int paddedRowBytes = ((rowBytes + 3) / 4) * 4; // 360*3 is already a multiple of 4
  int imageDataSize = paddedRowBytes * height;
  int fileSize = 54 + imageDataSize;

  uint8_t header[54];
  generateBMPHeader(header, width, height, imageDataSize);

  webServer_->setContentLength(fileSize);
  webServer_->sendHeader("Content-Type", "image/bmp");
  webServer_->sendHeader("Cache-Control", "no-cache");
  webServer_->send(200, "image/bmp", "");
  webServer_->client().write(header, 54);

  const uint16_t* buffer = source;
  uint8_t* rowBuffer = (uint8_t*)malloc(paddedRowBytes);
  if (!rowBuffer) {
    Serial.println("[TestInterface] out of memory building snapshot row");
    return;
  }

  // BMP rows are stored bottom-up.
  for (int y = height - 1; y >= 0; y--) {
    memset(rowBuffer, 0, paddedRowBytes);
    for (int x = 0; x < width; x++) {
      uint16_t px = buffer[y * width + x];
      uint8_t r5 = (px >> 11) & 0x1F;
      uint8_t g6 = (px >> 5) & 0x3F;
      uint8_t b5 = px & 0x1F;
      uint8_t r8 = (r5 << 3) | (r5 >> 2);
      uint8_t g8 = (g6 << 2) | (g6 >> 4);
      uint8_t b8 = (b5 << 3) | (b5 >> 2);
      rowBuffer[x * 3 + 0] = b8;
      rowBuffer[x * 3 + 1] = g8;
      rowBuffer[x * 3 + 2] = r8;
    }
    webServer_->client().write(rowBuffer, paddedRowBytes);
  }

  free(rowBuffer);
}
