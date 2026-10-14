#pragma once
#include <Arduino.h>

// This device's own description (DDF), compiled into flash from the board's
// ddf_zip.h. One translation unit includes that header, and everything else
// reads the bytes through here: the array is `static const` and ~300 KB,
// so each file including it directly would carry a copy of its own.
const uint8_t *ddfZipBytes();
size_t ddfZipSize();
