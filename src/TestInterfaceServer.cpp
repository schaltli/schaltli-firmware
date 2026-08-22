#include "TestInterfaceServer.h"
#include "ddf_zip.h"
#include "DeviceInfo.h"
#include "project/ProjectInstaller.h"
#include <ArduinoJson.h>
#include <Preferences.h>

bool TestInterfaceServer::begin(ClippedCanvas16& canvas, uint16_t port) {
  if (serverRunning_) return true;

  canvas_ = &canvas;
  webServer_ = new WebServer(port);
  webServer_->on("/snapshot.bmp", HTTP_GET, [this]() { this->handleSnapshot(); });
  webServer_->on("/ddf.zip", HTTP_GET, [this]() { this->handleDdfZip(); });
  webServer_->on("/api/screen", HTTP_POST, [this]() { this->handleScreenSwitch(); });
  webServer_->on("/api/mqtt", HTTP_POST, [this]() { this->handleMqttConfigure(); });
  webServer_->on("/api/forget-wifi", HTTP_POST, [this]() { this->handleForgetWiFi(); });
  webServer_->on("/api/topic-values", HTTP_GET, [this]() { this->handleGetTopicValues(); });
  webServer_->on("/api/input", HTTP_POST, [this]() { this->handleInput(); });
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
      LittleFS.remove("/temp_upload.zip");
      Serial.println("[TestInterface] zip too small");
      return;
    }

    bool ok = validateAndInstallZip();
    LittleFS.remove("/temp_upload.zip");

    if (ok) {
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
    Serial.printf("[TestInterface] project is system generation %ld.x, this firmware understands %d.%d - refusing\n",
                  uploadedMajor, SYSTEM_GENERATION_MAJOR, SYSTEM_GENERATION_MINOR);
    return false;
  }

  // Empty means the zip's project.json has no deviceId at all (older export,
  // or a non-project zip) - not proof it is wrong, so it is not rejected on
  // that basis alone.
  String uploadedDeviceId = ProjectInstaller::peekProjectDeviceId("/temp_upload.zip");
  if (!uploadedDeviceId.isEmpty() && uploadedDeviceId != DEVICE_ID) {
    Serial.printf("[TestInterface] deviceId mismatch: zip says \"%s\", this is \"%s\"\n",
                  uploadedDeviceId.c_str(), DEVICE_ID);
    return false;
  }

  String error;
  if (!ProjectInstaller::installProjectZipFromFile("/temp_upload.zip", error)) {
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
  sendBMP();
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

void TestInterfaceServer::sendBMP() {
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

  uint16_t* buffer = canvas_->getBuffer();
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
