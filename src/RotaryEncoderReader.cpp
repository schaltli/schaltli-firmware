#include "RotaryEncoderReader.h"
#include <driver/gpio.h>
#include <esp_timer.h>

namespace {

// Pin roles per Waveshare's demo (04_Encoder_Test/user_config.h:
// ECA_PIN=8 -> KNOB_RIGHT, ECB_PIN=7 -> KNOB_LEFT).
const gpio_num_t PIN_RIGHT = GPIO_NUM_8;
const gpio_num_t PIN_LEFT = GPIO_NUM_7;

// The demo's timing verbatim (TICKS_INTERVAL=3ms, DEBOUNCE_TICKS=2): a
// detent counts when its pin returns HIGH after reading LOW for at least
// one full poll tick. Polled from an esp_timer rather than edge
// interrupts because that is what the vendor reference does and it is
// hardware-verified; the timer task also keeps the cadence independent of
// however long loop() takes (the failure mode that bit the M5 Dial).
const uint64_t POLL_INTERVAL_US = 3000;
const uint8_t DEBOUNCE_TICKS = 2;

// Accumulated detents are shared between the esp_timer task (writer) and
// loop() (reader) - two tasks, potentially on different cores, hence the
// portMUX rather than a bare volatile.
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
int32_t pendingDetents = 0;

struct Channel {
  gpio_num_t pin;
  int8_t direction;
  uint8_t prevLevel;
  uint8_t debounceCnt;
};
Channel channels[2] = {
    {PIN_RIGHT, +1, 1, 0},
    {PIN_LEFT, -1, 1, 0},
};

// Structure kept identical to the demo's process_knob_channel() on
// purpose - it reads oddly (the debounce counter means "ticks spent LOW"
// in one branch and gates the rising edge in the other), but it is the
// logic that is known to work on this hardware, so it is replicated, not
// improved.
void pollChannel(Channel &ch) {
  uint8_t level = gpio_get_level(ch.pin) ? 1 : 0;
  if (level == 0) {
    if (level != ch.prevLevel) {
      ch.debounceCnt = 0;
    } else {
      ch.debounceCnt++;
    }
  } else {
    if (level != ch.prevLevel && ++ch.debounceCnt >= DEBOUNCE_TICKS) {
      ch.debounceCnt = 0;
      portENTER_CRITICAL(&mux);
      pendingDetents += ch.direction;
      portEXIT_CRITICAL(&mux);
    } else {
      ch.debounceCnt = 0;
    }
  }
  ch.prevLevel = level;
}

void pollCb(void *) {
  pollChannel(channels[0]);
  pollChannel(channels[1]);
}

esp_timer_handle_t timerHandle = nullptr;

}  // namespace

namespace RotaryEncoderReader {

void begin() {
  for (Channel &ch : channels) {
    pinMode(ch.pin, INPUT_PULLUP);
  }
  // Let the pull-ups settle before taking the initial levels, so a pin
  // still rising after power-up is not mistaken for a pulse in progress.
  delayMicroseconds(2000);
  for (Channel &ch : channels) {
    ch.prevLevel = gpio_get_level(ch.pin) ? 1 : 0;
    ch.debounceCnt = 0;
  }

  esp_timer_create_args_t args = {};
  args.callback = pollCb;
  args.dispatch_method = ESP_TIMER_TASK;
  args.name = "knob_poll";
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

}  // namespace RotaryEncoderReader
