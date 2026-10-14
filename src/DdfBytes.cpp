#include "DdfBytes.h"

// By name, not by relative path: the header wanted is the one the current
// build's include path provides (see project/DdfFontStore.cpp).
#include "ddf_zip.h"

const uint8_t *ddfZipBytes() { return DDF_ZIP; }
size_t ddfZipSize() { return sizeof(DDF_ZIP); }
