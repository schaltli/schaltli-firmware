#pragma once
#include <Arduino.h>

// A plain quadrature encoder - two channels a quarter period apart - as the
// LilyGO T-Encoder Pro has (GPIO 1/2). Not the knob's encoder, which pulses
// one channel per direction (RotaryEncoderReader.h); each board's
// BoardHardware.h picks its decoder.
//
// Polled from an esp_timer, like the knob's decoder and for its reason: the
// cadence stays independent of however long loop() takes. Every valid
// transition of the Gray code counts a step; an invalid one (both channels
// changed between two polls) counts nothing. stepsPerDetent steps make one
// detent - 2 on the T-Encoder Pro, measured with the probe of 2026-11-22.
namespace QuadratureEncoderReader {

void begin(uint8_t pinA, uint8_t pinB, uint8_t stepsPerDetent);

// Signed detents accumulated since the previous call (consume-on-read):
// positive = clockwise.
int32_t readDetents();

}
