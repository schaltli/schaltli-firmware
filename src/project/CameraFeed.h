#pragma once

#include <Arduino.h>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

class JPEGDEC;

// The picture of a Camera object (designer docs/2026-11-09-camera.md): one task
// fetches single images, the next when the last has arrived; another decodes
// only the newest. loop() takes finished pictures and draws them.
//
// Why two tasks and single images (spike on the 4", 2026-11-09). A panel reading
// an MJPEG stream showed a delay of 1 to 10 s: the sender outruns it and frames
// queue up on the way, where nothing can drop them. With one image in flight
// nothing can queue. The next is fetched once the last is decoded.
//
// Everything large sits in PSRAM. Internal RAM is what WiFi needs, and a large
// project leaves 25-31 KB of it (2026-10-30).
class CameraFeed {
public:
  static const int16_t MAX_W = 320;
  static const int16_t MAX_H = 240;
  static const size_t MAX_IMAGE_BYTES = 256 * 1024;  // spec decision 13

  // Before the fonts are parsed: they take what PSRAM there is (on the 4.3B, all
  // but 268 KB) and a Camera asking later would be refused. Safe to call again.
  bool allocate();

  // Starts fetching for one object. False when PSRAM or a task is not to be had.
  bool start(const String& id, const String& url, const String& username, const String& password, int16_t width,
             int16_t height);
  void stop();

  bool active() const { return active_.load(); }
  const String& objectId() const { return objectId_; }

  // A picture newer than the last one taken, or nullptr. Only for loop();
  // the pointer holds until the next call. Its rows are width() apart.
  const uint16_t* takeNewest();
  // The one taken last, for a redraw of the screen under it; nullptr: none.
  const uint16_t* shown() const { return shown_; }
  // Milliseconds since a picture was last finished; UINT32_MAX: none yet.
  uint32_t pictureAgeMs() const;
  // What is on the glass is too old to pass for live: a redraw must not bring it back.
  void forgetShown() { shown_ = nullptr; }
  int16_t width() const { return width_; }
  int16_t height() const { return height_; }

  String report() const;
  void resetStats();

  // The shortest time between two requests; 0: as fast as pictures are decoded.
  void setMinPeriodMs(uint32_t ms) { minPeriodMs_.store(ms); }
  uint32_t minPeriodMs() const { return minPeriodMs_.load(); }

private:
  struct Slot {
    uint8_t* data = nullptr;
    size_t capacity = 0;
    size_t length = 0;
  };
  struct Average {
    uint32_t count = 0, sumUs = 0, maxUs = 0;
    void add(uint32_t us) { count++; sumUs += us; if (us > maxUs) maxUs = us; }
  };

  static void fetchEntry(void* self);
  static void decodeEntry(void* self);
  void fetchLoop();
  void decodeLoop();
  void fetchFrom(const String& host, uint16_t port, const String& path, const String& authorization);
  bool grow(Slot& slot, size_t bytes);
  bool decode(const Slot& slot, uint16_t* target);
  static int onDraw(void* draw);

  std::atomic<bool> active_{false};
  std::atomic<uint32_t> minPeriodMs_{0};
  uint32_t lastRequestMs_ = 0;  // the fetch task's own
  // Bumped by start and stop: what is being fetched for an older one is dropped.
  std::atomic<uint32_t> generation_{0};
  SemaphoreHandle_t lock_ = nullptr;
  String url_;
  String authorization_;  // the whole header line, or empty
  String objectId_;
  int16_t width_ = 0, height_ = 0;

  TaskHandle_t fetchTask_ = nullptr, decodeTask_ = nullptr;
  JPEGDEC* jpeg_ = nullptr;

  // Three of each: one being written, one waiting, one being read; the newest
  // always wins. The top bit of the shared index says the slot holds news.
  static const uint8_t FRESH = 0x80;
  Slot images_[3];
  std::atomic<uint8_t> imageShared_{1};
  std::atomic<bool> decoding_{false};
  uint8_t imageWrite_ = 0;
  uint16_t* pictures_[3] = {nullptr, nullptr, nullptr};
  std::atomic<uint8_t> pictureShared_{1};
  uint8_t pictureRead_ = 2;
  const uint16_t* shown_ = nullptr;

  uint16_t* decodeTarget_ = nullptr;

  volatile uint32_t fetched_ = 0, decoded_ = 0, taken_ = 0, bad_ = 0, overwritten_ = 0, connects_ = 0, failedConnects_ = 0;
  volatile uint32_t lastPictureMs_ = 0;
  volatile uint32_t lastBytes_ = 0, sourceW_ = 0, sourceH_ = 0, scale_ = 1;
  Average fetch_, decode_;
  uint32_t statsSinceMs_ = 0;
};
