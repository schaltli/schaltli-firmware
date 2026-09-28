#include "ColorAssetLoader.h"
#include "RenderProfile.h"
#include <LittleFS.h>
#include <esp_heap_caps.h>
#include <vector>

namespace ColorAssetLoader {

namespace {
// Raw-bytes cache for drawGrayscaleMaskToCanvas() - added 2026-08-11 after
// ScreenNavigatorOverlay's continuous encoder-coupled scrolling made "smooth
// but laggy" visible on real hardware: with several tablets on-canvas at
// once, every single animation frame was re-opening and re-reading each
// one's PGM file from LittleFS from scratch, even though the same handful
// of icons repeat every frame while a project is loaded. Icon bytes never
// change at runtime, so caching them in RAM after the first read removes
// the flash I/O from the per-frame cost entirely - only the (cheap)
// per-pixel blend still runs every frame. A simple linear-scan vector, not
// a hash map: the number of distinct icon paths is bounded by screen count,
// which is small (tens, not thousands) for any project this firmware runs.
struct CacheEntry {
  String path;
  int32_t width = 0;
  int32_t height = 0;
  std::vector<uint8_t> gray;
};
std::vector<CacheEntry> maskCache;

// Dasselbe fuer BMPs, aus demselben Grund - nur dass es die hier bis
// 2026-08-24 nicht gab: drawBMPToCanvas() las die Datei bei JEDEM Aufruf neu.
// Beim Neuzeichnen eines Ausschnitts faellt das mehrfach an, einmal je
// Ausschnitt, und der Clip unterdrueckt nur das Schreiben, nicht das Lesen.
// Gemessen kosteten drei Ausschnitte auf einem Screen mit vier Symbolen
// dadurch 410ms - mehr als das Vollbild vorher.
//
// Fertig nach RGB565 gewandelt abgelegt, nicht als Rohbytes: die Umrechnung
// ist derselbe Aufwand fuer jedes Pixel und muss nur einmal passieren.
struct BmpCacheEntry {
  String path;
  int32_t width = 0;
  int32_t height = 0;
  std::vector<uint16_t> pixels;
};
std::vector<BmpCacheEntry> bmpCache;
size_t bmpCacheBytes = 0;

// Grosse Bilder bleiben ungecacht - ein bildschirmfuellender Hintergrund
// waere 259KB und verdraengt genau die kleinen Symbole, um die es geht.
const size_t BMP_CACHE_MAX_ENTRY = 32 * 1024;
const size_t BMP_CACHE_MAX_TOTAL = 192 * 1024;

// Ausser mit PSRAM, das alle drei Boards haben: dort passt auch ein Bild, das
// den ganzen Screen bedeckt - seit 2026-09-28 der Weg, einem Screen mehr als
// eine Farbe hinterzulegen (designer #16). Ungecacht las jede Aenderung eines
// Werts darueber das ganze Bild neu aus dem Flash, 260 KB auf dem Knob und
// 770 KB auf dem 4.3B, bis zehnmal pro Sekunde beim Ziehen eines Dimmers
// (designer #23). Grosse Eintraege kommen nur, wenn der Speicher wirklich frei
// ist, mit Reserve fuer alles andere.
const size_t BMP_CACHE_MAX_ENTRY_PSRAM = 2 * 1024 * 1024;
const size_t BMP_CACHE_MAX_TOTAL_PSRAM = 4 * 1024 * 1024;
const size_t BMP_CACHE_PSRAM_RESERVE = 512 * 1024;

bool mayCache(size_t bytes) {
  if (bytes <= BMP_CACHE_MAX_ENTRY && bmpCacheBytes + bytes <= BMP_CACHE_MAX_TOTAL) return true;
  if (!psramFound()) return false;
  if (bytes > BMP_CACHE_MAX_ENTRY_PSRAM || bmpCacheBytes + bytes > BMP_CACHE_MAX_TOTAL_PSRAM) return false;
  return heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) >= bytes + BMP_CACHE_PSRAM_RESERVE;
}

// Welche Zeilen und Spalten eines Bilds bei (x, y) auf dem Canvas ueberhaupt
// sichtbar sind: das Bild geschnitten mit dem Clip und dem Canvas selbst.
// Neu gezeichnet wird meist nur ein kleiner Ausschnitt, und drawPixel()
// verwirft zwar alles ausserhalb - aber erst, nachdem jedes Pixel des ganzen
// Bilds dafuer angefasst wurde. Die Grenzen sind halboffen: [c0, c1), [r0, r1).
bool visibleSpan(const ClippedCanvas16* canvas, int16_t x, int16_t y, int32_t w, int32_t h, int32_t& c0,
                 int32_t& c1, int32_t& r0, int32_t& r1) {
  int32_t left = 0, top = 0, right = canvas->width(), bottom = canvas->height();
  if (canvas->clipActive()) {
    left = canvas->clipX();
    top = canvas->clipY();
    right = left + canvas->clipW();
    bottom = top + canvas->clipH();
  }
  c0 = left - x > 0 ? left - x : 0;
  r0 = top - y > 0 ? top - y : 0;
  c1 = right - x < w ? right - x : w;
  r1 = bottom - y < h ? bottom - y : h;
  return c0 < c1 && r0 < r1;
}

// Returns the cached raw grayscale bytes for path, reading+caching from
// LittleFS on first request for that path. nullptr if the file can't be
// read or isn't a well-formed P5 PGM.
const CacheEntry* getCachedMask(const String& path) {
  for (const auto& entry : maskCache) {
    if (entry.path == path) return &entry;
  }

  File pgmFile = LittleFS.open(path, "r");
  if (!pgmFile) {
    Serial.printf("[ColorAssetLoader] Could not open \"%s\"\n", path.c_str());
    return nullptr;
  }

  String magic = pgmFile.readStringUntil('\n');
  magic.trim();
  if (magic != "P5") {
    Serial.printf("[ColorAssetLoader] \"%s\" is not a P5 PGM (bad magic \"%s\")\n", path.c_str(), magic.c_str());
    pgmFile.close();
    return nullptr;
  }

  // Skip comment lines ("#...") between the magic and the dimensions line -
  // same allowance the e-paper firmware's own PBM parser makes, even though
  // nothing in this pipeline actually emits comments today.
  String line;
  do {
    line = pgmFile.readStringUntil('\n');
    line.trim();
  } while (line.startsWith("#"));

  int spacePos = line.indexOf(' ');
  int32_t width = line.substring(0, spacePos).toInt();
  int32_t height = line.substring(spacePos + 1).toInt();

  // Maxval line - always "255" from bitmapToPGM(), the only producer of
  // this format today. Read and discard rather than hardcode-skip a fixed
  // byte count, so a stray comment line here (unlikely, but PGM technically
  // allows one) doesn't desync the parser.
  pgmFile.readStringUntil('\n');

  CacheEntry entry;
  entry.path = path;
  entry.width = width;
  entry.height = height;
  entry.gray.resize((size_t)width * (size_t)height);
  pgmFile.readBytes((char*)entry.gray.data(), entry.gray.size());
  pgmFile.close();

  maskCache.push_back(std::move(entry));
  return &maskCache.back();
}
}  // namespace

bool drawBMPToCanvas(ClippedCanvas16* canvas, const String& path, int16_t x, int16_t y) {
  if (path.isEmpty()) return false;

  for (const auto& hit : bmpCache) {
    if (hit.path != path) continue;
    RenderProfile::Scope s("  bmp blit (cached)");
    int32_t c0, c1, r0, r1;
    if (!visibleSpan(canvas, x, y, hit.width, hit.height, c0, c1, r0, r1)) return true;
    const uint16_t* px = hit.pixels.data();
    for (int32_t row = r0; row < r1; row++) {
      for (int32_t col = c0; col < c1; col++) {
        canvas->drawPixel(x + col, y + row, px[row * hit.width + col]);
      }
    }
    return true;
  }

  RenderProfile::Scope s("  bmp load (LittleFS)");
  File bmpFile = LittleFS.open(path, "r");
  if (!bmpFile) {
    Serial.printf("[ColorAssetLoader] Could not open \"%s\"\n", path.c_str());
    return false;
  }

  uint16_t signature = bmpFile.read() | (bmpFile.read() << 8);
  if (signature != 0x4D42) {  // 'BM'
    Serial.printf("[ColorAssetLoader] \"%s\" is not a BMP (bad signature)\n", path.c_str());
    bmpFile.close();
    return false;
  }

  bmpFile.seek(10);
  uint32_t dataOffset = bmpFile.read() | (bmpFile.read() << 8) | (bmpFile.read() << 16) | ((uint32_t)bmpFile.read() << 24);

  bmpFile.seek(18);
  int32_t width = bmpFile.read() | (bmpFile.read() << 8) | (bmpFile.read() << 16) | (bmpFile.read() << 24);
  int32_t height = bmpFile.read() | (bmpFile.read() << 8) | (bmpFile.read() << 16) | (bmpFile.read() << 24);

  bmpFile.seek(28);
  uint16_t bpp = bmpFile.read() | (bmpFile.read() << 8);

  if (bpp != 24 && bpp != 4) {
    Serial.printf("[ColorAssetLoader] \"%s\" is %dbpp, only 24bpp and 4bpp are supported\n",
                  path.c_str(), bpp);
    bmpFile.close();
    return false;
  }

  // 4bpp is what the designer exports for a device whose DDF declares
  // colorDepth "4bit" - the PaperS3, as of 2026-09-13. Until then every
  // colour board here was 24bit and this loader only knew that, so the first
  // conformance run on the PaperS3 failed every type that draws a bitmap and
  // passed every type that does not: icon, MQTTIconField, SoftwareButton,
  // and the tab-control and panel specimens because their children are
  // icons. Nothing was wrong with the rendering; the assets never arrived.
  //
  // The palette is 16 BGRA entries written straight after the DIB header
  // (lib/asset-export.ts's bitmapToBMP4Bit). It is always a grey ramp of
  // i*17, but it is read rather than assumed: a palette that says what the
  // pixels mean is the point of having one, and a future export that picks
  // different shades should not silently come out wrong here.
  uint16_t palette565[16] = {0};
  if (bpp == 4) {
    bmpFile.seek(14);
    const uint32_t dibSize = bmpFile.read() | (bmpFile.read() << 8) | (bmpFile.read() << 16) |
                             ((uint32_t)bmpFile.read() << 24);
    bmpFile.seek(14 + dibSize);
    for (int i = 0; i < 16; i++) {
      const uint8_t b = bmpFile.read();
      const uint8_t g = bmpFile.read();
      const uint8_t r = bmpFile.read();
      bmpFile.read();  // reserved
      palette565[i] = ((uint16_t)(r & 0xF8) << 8) | ((uint16_t)(g & 0xFC) << 3) | (b >> 3);
    }
  }

  // Both formats pad rows to a 4-byte boundary; 4bpp packs two pixels per
  // byte, high nibble first.
  uint32_t rowSize = (bpp == 24) ? (((uint32_t)width * 3 + 3) / 4) * 4
                                 : ((((uint32_t)width + 1) / 2 + 3) / 4) * 4;

  bool flip = height > 0;  // positive height = bottom-up (the normal case)
  if (height < 0) height = -height;

  uint8_t* rowBuffer = (uint8_t*)malloc(rowSize);
  if (!rowBuffer) {
    Serial.println("[ColorAssetLoader] rowBuffer malloc failed");
    bmpFile.close();
    return false;
  }

  // Beim ersten Zeichnen gleich mitschreiben, statt die Datei spaeter ein
  // zweites Mal zu lesen.
  const size_t braucht = (size_t)width * (size_t)height * sizeof(uint16_t);
  const bool cachen = mayCache(braucht);
  BmpCacheEntry neu;
  if (cachen) {
    neu.path = path;
    neu.width = width;
    neu.height = height;
    neu.pixels.resize((size_t)width * (size_t)height);
  }

  // Wird das Bild gecacht, einmal ganz gelesen. Sonst nur die Zeilen und
  // Spalten, die der Ausschnitt braucht - der Rest des Bilds kostet dann
  // nichts, auch kein Lesen aus dem Flash.
  int32_t c0 = 0, c1 = width, r0 = 0, r1 = height;
  const bool sichtbar = visibleSpan(canvas, x, y, width, height, c0, c1, r0, r1);
  if (cachen) {
    c0 = 0;
    c1 = width;
    r0 = 0;
    r1 = height;
  } else if (!sichtbar) {
    r1 = r0;
  }

  for (int32_t row = r0; row < r1; row++) {
    int32_t readRow = flip ? (height - 1 - row) : row;
    bmpFile.seek(dataOffset + (readRow * rowSize));
    bmpFile.readBytes((char*)rowBuffer, rowSize);

    for (int32_t col = c0; col < c1; col++) {
      uint16_t rgb565;
      if (bpp == 24) {
        uint8_t b = rowBuffer[col * 3];
        uint8_t g = rowBuffer[col * 3 + 1];
        uint8_t r = rowBuffer[col * 3 + 2];
        rgb565 = ((uint16_t)(r & 0xF8) << 8) | ((uint16_t)(g & 0xFC) << 3) | (b >> 3);
      } else {
        const uint8_t packed = rowBuffer[col / 2];
        rgb565 = palette565[(col & 1) ? (packed & 0x0F) : (packed >> 4)];
      }
      if (cachen) neu.pixels[(size_t)row * (size_t)width + (size_t)col] = rgb565;
      canvas->drawPixel(x + col, y + row, rgb565);
    }
  }

  if (cachen) {
    bmpCacheBytes += braucht;
    bmpCache.push_back(std::move(neu));
  }

  free(rowBuffer);
  bmpFile.close();
  return true;
}

bool drawGrayscaleMaskToCanvas(ClippedCanvas16* canvas, const String& path, int16_t x, int16_t y, uint16_t fgColor, uint16_t bgColor) {
  if (path.isEmpty()) return false;

  const CacheEntry* entry = getCachedMask(path);
  if (!entry) return false;

  // Unpacked once, outside the pixel loop - RGB565: bits [15:11]=R5,
  // [10:5]=G6, [4:0]=B5.
  uint16_t fgR = (fgColor >> 11) & 0x1F, fgG = (fgColor >> 5) & 0x3F, fgB = fgColor & 0x1F;
  uint16_t bgR = (bgColor >> 11) & 0x1F, bgG = (bgColor >> 5) & 0x3F, bgB = bgColor & 0x1F;

  int32_t c0, c1, r0, r1;
  if (!visibleSpan(canvas, x, y, entry->width, entry->height, c0, c1, r0, r1)) return true;
  for (int32_t row = r0; row < r1; row++) {
    for (int32_t col = c0; col < c1; col++) {
      uint8_t gray = entry->gray[(size_t)row * entry->width + col];  // 0 = full fgColor, 255 = full bgColor (see convertToGrayscale())
      if (gray == 255) continue;  // fully background - skip, the tablet fill already drawn there is correct as-is
      uint16_t r = (fgR * (255 - gray) + bgR * gray) / 255;
      uint16_t g = (fgG * (255 - gray) + bgG * gray) / 255;
      uint16_t b = (fgB * (255 - gray) + bgB * gray) / 255;
      canvas->drawPixel(x + col, y + row, (r << 11) | (g << 5) | b);
    }
  }

  return true;
}

// Leert BEIDE Caches. Sie haengen an denselben Icon-Pfaden: die sind
// screen-id-basiert und nicht inhaltsgehasht, ein Redeploy kann also die
// Bytes hinter einem Pfad austauschen. Wer nur einen leert, zeigt danach
// aus dem anderen alte Pixel.
void clearGrayscaleMaskCache() {
  maskCache.clear();
  bmpCache.clear();
  bmpCacheBytes = 0;
}

}  // namespace ColorAssetLoader
