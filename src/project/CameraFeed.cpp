#include "CameraFeed.h"

#include <JPEGDEC.h>
#include <WiFi.h>
#include <base64.h>
#include <esp_heap_caps.h>
#include <new>

// The first size of an image slot; a slot grows to what a camera sends.
static const size_t INITIAL_SLOT_BYTES = 32 * 1024;
static const uint32_t RESPONSE_TIMEOUT_MS = 3000;
static const uint32_t TASK_STACK_BYTES = 8192;

static CameraFeed* drawing = nullptr;  // JPEGDEC's callback takes no context of ours

namespace {
struct Url {
  String host, path;
  uint16_t port = 80;
};

// One header line, giving way while it waits. Stream::readStringUntil spins on
// millis() without yielding: a camera slow to answer starved core 0's idle task
// until the task watchdog restarted the board (HIL, 2026-11-09).
bool readLine(WiFiClient& client, String& line, uint32_t timeoutMs) {
  static const size_t MAX_LINE = 512;
  line = "";
  uint32_t lastData = millis();
  while (millis() - lastData < timeoutMs) {
    if (client.available() > 0) {
      const int c = client.read();
      if (c < 0) continue;
      lastData = millis();
      if (c == '\n') return true;
      if (line.length() < MAX_LINE) line += (char)c;
    } else if (!client.connected()) {
      return false;
    } else {
      delay(1);
    }
  }
  return false;
}

bool parseUrl(const String& url, Url& out) {
  if (!url.startsWith("http://")) return false;
  const int start = 7;
  int slash = url.indexOf('/', start);
  if (slash < 0) slash = url.length();
  const String authority = url.substring(start, slash);
  out.path = slash < (int)url.length() ? url.substring(slash) : String("/");
  const int colon = authority.indexOf(':');
  out.port = colon >= 0 ? (uint16_t)authority.substring(colon + 1).toInt() : 80;
  out.host = colon >= 0 ? authority.substring(0, colon) : authority;
  return out.host.length() > 0 && out.port > 0;
}
}  // namespace

bool CameraFeed::grow(Slot& slot, size_t bytes) {
  if (bytes <= slot.capacity) return true;
  void* grown = heap_caps_realloc(slot.data, bytes, MALLOC_CAP_SPIRAM);
  if (grown == nullptr) return false;
  slot.data = (uint8_t*)grown;
  slot.capacity = bytes;
  return true;
}

bool CameraFeed::allocate() {
  if (jpeg_ == nullptr) {
    void* memory = heap_caps_malloc(sizeof(JPEGDEC), MALLOC_CAP_SPIRAM);
    if (memory == nullptr) return false;
    jpeg_ = new (memory) JPEGDEC();
  }
  for (Slot& slot : images_)
    if (!grow(slot, INITIAL_SLOT_BYTES)) return false;
  for (uint16_t*& picture : pictures_) {
    if (picture != nullptr) continue;
    picture = (uint16_t*)heap_caps_malloc((size_t)MAX_W * MAX_H * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (picture == nullptr) return false;
  }
  return true;
}

bool CameraFeed::start(const String& id, const String& url, const String& username, const String& password, int16_t width,
                       int16_t height) {
  if (lock_ == nullptr) lock_ = xSemaphoreCreateMutex();
  if (lock_ == nullptr || !allocate()) return false;

  xSemaphoreTake(lock_, portMAX_DELAY);
  url_ = url;
  authorization_ = username.isEmpty() ? String() : "Authorization: Basic " + base64::encode(username + ":" + password) + "\r\n";
  objectId_ = id;
  width_ = min(width, MAX_W);
  height_ = min(height, MAX_H);
  xSemaphoreGive(lock_);

  shown_ = nullptr;
  lastPictureMs_ = 0;
  generation_++;
  imageShared_.fetch_and((uint8_t)~FRESH);
  pictureShared_.fetch_and((uint8_t)~FRESH);
  active_.store(true);

  // Once, on first use: a project without a Camera pays nothing for this.
  // Beside WiFi on core 0, away from the loop's drawing on core 1.
  if (fetchTask_ == nullptr &&
      xTaskCreatePinnedToCore(fetchEntry, "camfetch", TASK_STACK_BYTES, this, 1, &fetchTask_, 0) != pdPASS)
    return false;
  if (decodeTask_ == nullptr &&
      xTaskCreatePinnedToCore(decodeEntry, "camdecode", TASK_STACK_BYTES, this, 1, &decodeTask_, 0) != pdPASS)
    return false;
  return true;
}

void CameraFeed::stop() {
  active_.store(false);
  generation_++;
  shown_ = nullptr;
}

const uint16_t* CameraFeed::takeNewest() {
  if (!active_.load() || !(pictureShared_.load() & FRESH)) return nullptr;
  pictureRead_ = pictureShared_.exchange(pictureRead_) & 0x7F;
  taken_ = taken_ + 1;
  shown_ = pictures_[pictureRead_];
  return shown_;
}

void CameraFeed::fetchEntry(void* self) { static_cast<CameraFeed*>(self)->fetchLoop(); }
void CameraFeed::decodeEntry(void* self) { static_cast<CameraFeed*>(self)->decodeLoop(); }

void CameraFeed::fetchLoop() {
  for (;;) {
    if (!active_.load() || WiFi.status() != WL_CONNECTED) {
      delay(50);
      continue;
    }
    xSemaphoreTake(lock_, portMAX_DELAY);
    const String url = url_;
    const String authorization = authorization_;
    xSemaphoreGive(lock_);
    Url parsed;
    if (!parseUrl(url, parsed)) {
      bad_ = bad_ + 1;
      delay(1000);
      continue;
    }
    fetchFrom(parsed.host, parsed.port, parsed.path, authorization);
    delay(1);
  }
}

// Reads images off one connection for as long as the server keeps it open and
// this generation stands. IP Webcam closes after every image; a server that
// does not saves a connection per image.
void CameraFeed::fetchFrom(const String& host, uint16_t port, const String& path, const String& authorization) {
  const uint32_t generation = generation_.load();
  WiFiClient client;
  client.setTimeout(RESPONSE_TIMEOUT_MS);
  if (!client.connect(host.c_str(), port, RESPONSE_TIMEOUT_MS)) {
    failedConnects_ = failedConnects_ + 1;
    delay(250);
    return;
  }
  client.setNoDelay(true);
  connects_ = connects_ + 1;

  while (client.connected() && generation_.load() == generation) {
    // The next image only once the last is decoded. Fetching ahead bought
    // nothing: 19 images a second fetched, 3.3 shown, the rest thrown away at
    // the cost of the core and the air the decoder needed (HIL on the 4",
    // 2026-11-09; decode went from 40-90 to 195 ms). It also makes the picture
    // fresher: what is decoded left the camera a moment ago.
    while ((imageShared_.load() & FRESH) || decoding_.load()) {
      if (generation_.load() != generation) return;
      delay(1);
    }
    // A board whose loop cannot spare more pictures asks for them no sooner.
    while (minPeriodMs_.load() > 0 && millis() - lastRequestMs_ < minPeriodMs_.load()) {
      if (generation_.load() != generation) return;
      delay(5);
    }
    lastRequestMs_ = millis();
    const uint32_t t0 = micros();
    client.printf("GET %s HTTP/1.1\r\nHost: %s\r\n%sConnection: keep-alive\r\n\r\n", path.c_str(), host.c_str(), authorization.c_str());

    String status;
    if (!readLine(client, status, RESPONSE_TIMEOUT_MS)) break;  // an idle connection the server dropped
    long contentLength = -1;
    bool closes = false;
    for (;;) {
      String header;
      if (!readLine(client, header, RESPONSE_TIMEOUT_MS)) return;
      header.trim();
      if (header.isEmpty()) break;
      header.toLowerCase();
      if (header.startsWith("content-length:")) contentLength = header.substring(15).toInt();
      else if (header.startsWith("connection:") && header.indexOf("close") >= 0) closes = true;
    }
    if (contentLength < 0) closes = true;  // the end of the image is the end of the connection
    if (status.indexOf(" 200") < 0 || contentLength > (long)MAX_IMAGE_BYTES) {
      bad_ = bad_ + 1;
      delay(250);
      return;
    }

    // To Content-Length and not one byte longer: waiting for the server to
    // close cost the spike more than half its frame rate (4-5 against 11 fps).
    Slot& slot = images_[imageWrite_];
    if (contentLength >= 0 && !grow(slot, (size_t)contentLength)) {
      bad_ = bad_ + 1;
      return;
    }
    const size_t target = contentLength >= 0 ? (size_t)contentLength : MAX_IMAGE_BYTES;
    size_t length = 0;
    uint32_t lastData = millis();
    while (length < target && millis() - lastData < RESPONSE_TIMEOUT_MS && generation_.load() == generation) {
      const int available = client.available();
      if (available > 0) {
        if (length == slot.capacity && !grow(slot, min(slot.capacity * 2, MAX_IMAGE_BYTES))) break;
        const int n = client.read(slot.data + length, min((size_t)available, slot.capacity - length));
        if (n > 0) {
          length += n;
          lastData = millis();
        }
      } else if (!client.connected()) {
        break;
      } else {
        delay(1);
      }
    }
    if (generation_.load() != generation) return;
    const bool complete = contentLength >= 0 ? length == target : length > 0 && length < MAX_IMAGE_BYTES;
    if (!complete) {
      bad_ = bad_ + 1;
      return;
    }

    fetch_.add(micros() - t0);
    fetched_ = fetched_ + 1;
    lastBytes_ = length;
    slot.length = length;
    const uint8_t previous = imageShared_.exchange((uint8_t)(imageWrite_ | FRESH));
    if (previous & FRESH) overwritten_ = overwritten_ + 1;
    imageWrite_ = previous & 0x7F;
    if (closes) return;
  }
}

void CameraFeed::decodeLoop() {
  uint8_t read = 2, write = 0;
  for (;;) {
    if (!(imageShared_.load() & FRESH)) {
      delay(1);
      continue;
    }
    // Every image, not only when there is nothing to do: with images arriving
    // as fast as they are decoded there never is nothing, and core 0's idle
    // task starves until the task watchdog restarts the board (HIL, 2026-11-09).
    delay(1);
    decoding_.store(true);
    read = imageShared_.exchange(read) & 0x7F;
    const uint32_t generation = generation_.load();
    const uint32_t t0 = micros();
    const bool decoded = decode(images_[read], pictures_[write]);
    decoding_.store(false);
    if (!decoded) {
      bad_ = bad_ + 1;
      continue;
    }
    decode_.add(micros() - t0);
    decoded_ = decoded_ + 1;
    if (generation_.load() != generation) continue;
    lastPictureMs_ = millis();
    write = pictureShared_.exchange((uint8_t)(write | FRESH)) & 0x7F;
  }
}

int CameraFeed::onDraw(void* drawArg) {
  JPEGDRAW* d = static_cast<JPEGDRAW*>(drawArg);
  CameraFeed* self = drawing;
  const int16_t W = self->width_, H = self->height_;
  for (int row = 0; row < d->iHeight; row++) {
    const int y = d->y + row;
    if (y < 0 || y >= H || d->x >= W) continue;
    const int columns = min<int>(d->iWidth, W - d->x);
    memcpy(self->decodeTarget_ + (size_t)y * W + d->x, d->pPixels + (size_t)row * d->iWidth, (size_t)columns * sizeof(uint16_t));
  }
  return 1;
}

// Fitted whole and centred: a larger source scaled down while decoding, a
// smaller one not enlarged (spec decision 8).
bool CameraFeed::decode(const Slot& slot, uint16_t* target) {
  drawing = this;
  decodeTarget_ = target;
  if (!jpeg_->openRAM(slot.data, (int)slot.length, reinterpret_cast<JPEG_DRAW_CALLBACK*>(onDraw))) return false;
  jpeg_->setPixelType(RGB565_LITTLE_ENDIAN);
  const int w = jpeg_->getWidth(), h = jpeg_->getHeight();
  int shift = 0, options = 0;
  if (w > width_ * 4 || h > height_ * 4) { shift = 3; options = JPEG_SCALE_EIGHTH; }
  else if (w > width_ * 2 || h > height_ * 2) { shift = 2; options = JPEG_SCALE_QUARTER; }
  else if (w > width_ || h > height_) { shift = 1; options = JPEG_SCALE_HALF; }
  sourceW_ = w;
  sourceH_ = h;
  scale_ = 1u << shift;
  const int offsetX = max(0, (width_ - (w >> shift)) / 2);
  const int offsetY = max(0, (height_ - (h >> shift)) / 2);
  if (offsetX > 0 || offsetY > 0) memset(target, 0, (size_t)width_ * height_ * sizeof(uint16_t));
  const bool ok = jpeg_->decode(offsetX, offsetY, options) != 0;
  jpeg_->close();
  return ok;
}

uint32_t CameraFeed::pictureAgeMs() const {
  const uint32_t at = lastPictureMs_;
  return at == 0 ? UINT32_MAX : millis() - at;
}

void CameraFeed::resetStats() {
  fetched_ = decoded_ = taken_ = bad_ = overwritten_ = connects_ = failedConnects_ = 0;
  fetch_ = Average();
  decode_ = Average();
  statsSinceMs_ = millis();
}

String CameraFeed::report() const {
  const float seconds = max(0.001f, (millis() - statsSinceMs_) / 1000.0f);
  const float fetchMs = fetch_.count ? fetch_.sumUs / (float)fetch_.count / 1000.0f : 0;
  const float decodeMs = decode_.count ? decode_.sumUs / (float)decode_.count / 1000.0f : 0;
  String out = "camera: ";
  out += active_.load() ? "fetching" : "idle";
  out += ", fetched/s " + String(fetched_ / seconds, 1);
  out += ", decoded/s " + String(decoded_ / seconds, 1);
  out += ", shown/s " + String(taken_ / seconds, 1);
  out += ", fetch avg ms " + String(fetchMs, 1) + " max " + String(fetch_.maxUs / 1000.0f, 1);
  out += ", decode avg ms " + String(decodeMs, 1) + " max " + String(decode_.maxUs / 1000.0f, 1);
  out += ", bytes " + String((unsigned)lastBytes_);
  out += ", source " + String((unsigned)sourceW_) + "x" + String((unsigned)sourceH_) + " /" + String((unsigned)scale_);
  out += ", bad " + String((unsigned)bad_) + ", overwritten " + String((unsigned)overwritten_);
  out += ", connects " + String((unsigned)connects_) + " failed " + String((unsigned)failedConnects_);
  out += ", stack free fetch " + String(fetchTask_ ? uxTaskGetStackHighWaterMark(fetchTask_) : 0);
  out += " decode " + String(decodeTask_ ? uxTaskGetStackHighWaterMark(decodeTask_) : 0);
  return out;
}
