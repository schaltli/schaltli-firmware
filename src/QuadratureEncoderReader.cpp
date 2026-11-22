#include "QuadratureEncoderReader.h"
#include <driver/gpio.h>
#include <esp_timer.h>

namespace {

// 1 ms: a fast turn on the T-Encoder Pro's ring gives a step every few
// milliseconds, and a missed one in a quadrature code is a lost step, not a
// late one.
const uint64_t POLL_INTERVAL_US = 1000;

gpio_num_t pinA = GPIO_NUM_NC, pinB = GPIO_NUM_NC;
int8_t stepsPerDetent = 2;
uint8_t state = 0;
// Steps not yet making a whole detent, written by the timer task only.
int8_t partial = 0;

// Shared between the esp_timer task (writer) and loop() (reader).
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
int32_t pendingDetents = 0;

// Indexed by (previous state << 2) | new state, state = A << 1 | B: +1 or -1
// for the four valid transitions each way, 0 for none and for the invalid
// double changes.
const int8_t TRANSITION[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};

uint8_t readState() {
  return (uint8_t)((gpio_get_level(pinA) ? 2 : 0) | (gpio_get_level(pinB) ? 1 : 0));
}

void pollCb(void *) {
  const uint8_t now = readState();
  const int8_t step = TRANSITION[(state << 2) | now];
  state = now;
  if (step == 0) return;
  partial += step;
  if (partial >= stepsPerDetent || partial <= -stepsPerDetent) {
    const int32_t detent = partial > 0 ? 1 : -1;
    partial = 0;
    portENTER_CRITICAL(&mux);
    pendingDetents += detent;
    portEXIT_CRITICAL(&mux);
  }
}

esp_timer_handle_t timerHandle = nullptr;

}  // namespace

namespace QuadratureEncoderReader {

void begin(uint8_t a, uint8_t b, uint8_t steps) {
  pinA = (gpio_num_t)a;
  pinB = (gpio_num_t)b;
  stepsPerDetent = (int8_t)(steps == 0 ? 1 : steps);
  pinMode(a, INPUT_PULLUP);
  pinMode(b, INPUT_PULLUP);
  delayMicroseconds(2000);
  state = readState();
  partial = 0;

  esp_timer_create_args_t args = {};
  args.callback = pollCb;
  args.dispatch_method = ESP_TIMER_TASK;
  args.name = "quad_poll";
  if (esp_timer_create(&args, &timerHandle) == ESP_OK) {
    esp_timer_start_periodic(timerHandle, POLL_INTERVAL_US);
  }
}

int32_t readDetents() {
  portENTER_CRITICAL(&mux);
  int32_t ret = pendingDetents;
  pendingDetents = 0;
  portEXIT_CRITICAL(&mux);
  return ret;
}

}  // namespace QuadratureEncoderReader
