// The cycle counter the rasterizer's diagnostic probe reads. Off the device
// there is nothing to count, and the probe is off by default anyway.
#pragma once
#include <stdint.h>
static inline uint32_t esp_cpu_get_cycle_count(void) { return 0; }
