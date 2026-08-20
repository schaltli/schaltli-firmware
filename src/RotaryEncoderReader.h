#pragma once
#include <Arduino.h>

// This board's knob is NOT a quadrature encoder, despite having two
// channels. Waveshare's own demo decodes it as a bidirectional PULSE
// encoder (ESP32-S3-Knob-Touch-LCD-1.8-Demo.zip,
// Arduino/examples/04_Encoder_Test/bidi_switch_knob.c - a modified
// espressif iot_knob component whose modification is exactly this): each
// channel is evaluated independently, a debounced LOW pulse on GPIO8 is one
// detent RIGHT, on GPIO7 one detent LEFT. Both idle HIGH; the opposite
// channel stays HIGH while turning.
//
// Found 2026-08-20, replacing a Gray-code quadrature decoder ported from
// the M5 Dial (whose encoder really is quadrature). That model mismatch
// explains everything the pin instrumentation showed on this board:
// - edge counts split ~7:1 between the channels (a quadrature encoder
//   transitions both equally; a pulse encoder only pulses the channel for
//   the direction being turned),
// - one channel "stuck" HIGH while the other toggled (that IS the
//   protocol - the stuck one belongs to the direction not being turned),
// - the quadrature state machine emitting +1/-1 jitter that never
//   accumulated (fed a single toggling channel, that is all it can emit).
namespace RotaryEncoderReader {

void begin();

// Signed detents accumulated since the previous call (consume-on-read):
// positive = right (GPIO8 pulses), negative = left (GPIO7 pulses).
int32_t readDetents();

}
