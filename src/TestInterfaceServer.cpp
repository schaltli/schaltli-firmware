#include "TestInterfaceServer.h"
#include "ddf_zip.h"

bool TestInterfaceServer::begin(ClippedCanvas16& canvas, uint16_t port) {
  if (serverRunning_) return true;

  canvas_ = &canvas;
  webServer_ = new WebServer(port);
  webServer_->on("/snapshot.bmp", HTTP_GET, [this]() { this->handleSnapshot(); });
  webServer_->on("/ddf.zip", HTTP_GET, [this]() { this->handleDdfZip(); });
  webServer_->begin();
  serverRunning_ = true;

  Serial.printf("[TestInterface] listening on port %u: /snapshot.bmp, /ddf.zip\n", port);
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
