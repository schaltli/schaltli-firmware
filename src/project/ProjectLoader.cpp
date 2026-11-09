#include "ProjectLoader.h"
#include "LiveValueJson.h"
#include "ThemeVariant.h"
#include "TextField.h"
#include "ButtonValues.h"
#include <exception>
#include <math.h>

namespace {
// Every asset path stored in project.json (icon `path`, SoftwareButton
// `iconPath`/`iconPathPressed`, MQTTIconField `valueIconPairs[].path`, a
// screen's flattened-background `path`) is written by the designer's
// export pipeline (lib/project-zip.ts) relative to the *zip root* (e.g.
// "assets/icon-lock.bmp") - the same convention e-paper's own export uses.
// ProjectInstaller::installProjectZipFromFile() always installs every zip
// entry under "/PROJECT/" (e.g. "/PROJECT/assets/icon-lock.bmp"), but
// nothing was ever prefixing these path fields to match before they
// reached ColorAssetLoader::drawBMPToCanvas() (a literal LittleFS.open()
// on whatever string it's given) - every icon/SoftwareButton/MQTTIconField
// silently fell into its own "asset missing" black-square placeholder,
// found 2026-08-10 building the M5 Dial HIL fixture (both a plain `icon`
// and an MQTTIconField rendered as solid black squares instead of their
// real bitmaps - the *load failure* fallback, not a pixel-content bug).
String resolveAssetPath(const String& path) {
  if (path.isEmpty() || path.charAt(0) == '/') return path;
  return "/PROJECT/" + path;
}

// Das Projektdokument aus dem PSRAM statt aus dem internen Heap.
//
// Warum (2026-08-26): der dritte Screen liess das Panel mit "no project yet"
// stehen. project.json war von 20.863 auf 37.922 Bytes gewachsen, der alte
// Code forderte dafuer das 1,5-fache AM STUECK aus dem internen Heap -
// 56.883 Bytes gegen gemessene 43.036 frei. Kein Fehler im Projekt: es
// passte schlicht nicht mehr.
//
// Der interne Heap ist auf diesem Board die knappe Ressource (LVGL-Puffer,
// WiFi, TLS), waehrend 8 MB PSRAM danebenliegen und fuer Panel-Schatten und
// Schriften laengst genutzt werden. Ein Projektdokument gehoert dorthin: es
// wird einmal beim Laden aufgebaut, nicht in einem Interrupt angefasst, und
// die etwas langsameren Zugriffe fallen gegen einen Dateiparse nicht ins
// Gewicht.
//
// ArduinoJson 7 laesst das Dokument ausserdem von selbst wachsen. Damit
// entfaellt die ganze Schaetzerei - das feste Vorabmass, der 4096er Boden
// und die ESP.getMaxAllocHeap()-Pruefung, die dieses Board schon einmal
// komplett blockiert hatte (siehe die Notiz vom 2026-08-10 weiter unten in
// der Versionsgeschichte).
//
// Faellt PSRAM aus irgendeinem Grund aus, wird auf den normalen Heap
// zurueckgegriffen - dann gilt wieder die alte Grenze, aber das Geraet
// laedt, statt gar nichts zu tun.
struct PsramAllocator : ArduinoJson::Allocator {
  void* allocate(size_t size) override {
    void* p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    return p ? p : malloc(size);
  }
  void deallocate(void* ptr) override {
    heap_caps_free(ptr);  // kennt beide Herkuenfte
  }
  void* reallocate(void* ptr, size_t new_size) override {
    void* p = heap_caps_realloc(ptr, new_size, MALLOC_CAP_SPIRAM);
    return p ? p : realloc(ptr, new_size);
  }
};

PsramAllocator& psramAllocator() {
  static PsramAllocator allocator;
  return allocator;
}
}  // namespace

ProjectLoader::ProjectLoader() : loaded_(false) {}

bool ProjectLoader::loadProject(const String& projectPath) {
  if (!LittleFS.exists(projectPath)) {
    return false;
  }
  
  File file = LittleFS.open(projectPath, "r");
  if (!file) {
    return false;
  }
  
  // Parse directly from file stream instead of loading entire file into String
  // This avoids doubling memory usage (String + JsonDocument)
  bool success = parseJSONFromFile(file);
  file.close();
  
  if (success) {
    loaded_ = true;
  }
  
  return success;
}

bool ProjectLoader::parseJSONFromFile(File& file) {
  // Aus dem Dateistrom, damit die Datei nicht zusaetzlich als String im
  // Speicher liegt - und in ein Dokument, das aus dem PSRAM waechst.
  //
  // Was hier bis 2026-08-26 stand und warum es weg ist: eine Schaetzung
  // (Dateigroesse x 1,5), ein Mindestmass und eine Pruefung gegen
  // ESP.getMaxAllocHeap(). Diese Kette hatte das Board schon zweimal
  // blockiert. Beim ersten Mal (2026-08-10) war das Mindestmass von 32768
  // unconditional groesser als der hier verfuegbare Block, was JEDEN
  // Projektladevorgang scheitern liess - auch den einer 598-Byte-Vorlage.
  // Beim zweiten Mal (2026-08-26) sprengte der dritte Screen die Grenze:
  // 56.883 Bytes gefordert, 43.036 frei, das Panel blieb auf "no project
  // yet" stehen. Beide Male war nicht das Projekt zu gross, sondern die
  // Annahme falsch.
  //
  // ArduinoJson 7 laesst das Dokument bedarfsgerecht wachsen. Es gibt also
  // nichts mehr zu schaetzen, und mit PsramAllocator kommt der Speicher aus
  // den 8 MB PSRAM statt aus dem knappen internen Heap.
  JsonDocument doc(&psramAllocator());

  DeserializationError error = deserializeJson(doc, file,
                                                DeserializationOption::NestingLimit(30));

  if (error) {
    Serial.printf("[ProjectLoader] %s laesst sich nicht parsen: %s (%u Bytes)\n",
                  file.name(), error.c_str(), (unsigned)file.size());
    return false;
  }

  // Bleibt: parseScreens()' reserve() kann weiterhin std::bad_alloc werfen,
  // wenn selbst eine einzelne richtig bemessene Zuteilung nicht passt. Dann
  // soll dieser eine Ladevorgang scheitern und nicht das Geraet.
  try {
    return parseJSONDocument(doc);
  } catch (const std::exception& e) {
    Serial.printf("[ProjectLoader] Exception while parsing %s: %s\n", file.name(), e.what());
    return false;
  }
}

bool ProjectLoader::parseJSON(const String& jsonContent) {
  // Gleiche Begruendung wie in parseJSONFromFile(): aus dem PSRAM, ohne
  // Vorabmass, weil ArduinoJson 7 selbst waechst.
  JsonDocument doc(&psramAllocator());

  DeserializationError error = deserializeJson(doc, jsonContent,
                                                DeserializationOption::NestingLimit(30));

  if (error) {
    Serial.printf("[ProjectLoader] Projekt-JSON laesst sich nicht parsen: %s (%u Bytes)\n",
                  error.c_str(), (unsigned)jsonContent.length());
    return false;
  }

  // See parseJSONFromFile()'s identical try/catch for why.
  try {
    return parseJSONDocument(doc);
  } catch (const std::exception& e) {
    Serial.printf("[ProjectLoader] Exception while parsing project JSON: %s\n", e.what());
    return false;
  }
}

bool ProjectLoader::parseJSONDocument(JsonDocument& doc) {
  // Parse basic project info
  project_.name = doc["name"] | "Unnamed Project";
  project_.screenWidth = doc["screenWidth"] | 300;
  project_.screenHeight = doc["screenHeight"] | 400;
  project_.exportColorDepth = doc["exportColorDepth"] | "1bit";
  project_.decimalSeparator = doc["decimalSeparator"] | ".";
  project_.thousandsSeparator = doc["thousandsSeparator"] | "'";
  
  // Parse topics
  if (doc["topics"].is<JsonArray>()) {
    parseTopics(doc["topics"].as<JsonArray>());
  }
  
  // Parse fonts
  if (doc["fonts"].is<JsonArray>()) {
    parseFonts(doc["fonts"].as<JsonArray>());
  }
  
  // Popups before the screens only by line; either order reads the same. A
  // project without them leaves none from an earlier load behind.
  project_.popups.clear();
  project_.popupFence = PopupFence();
  if (doc["popups"].is<JsonArray>()) {
    parsePopups(doc["popups"].as<JsonArray>(), doc["popupFence"].as<JsonObject>());
  }
  // Parse screens
  if (doc["screens"].is<JsonArray>()) {
    parseScreens(doc["screens"].as<JsonArray>());
  }

  // Navigators (contract 2.7): their entries are ordinary objects.
  project_.navigators.clear();
  if (doc["navigators"].is<JsonArray>()) {
    for (JsonObject navJson : doc["navigators"].as<JsonArray>()) {
      Navigator nav;
      nav.id = navJson["id"] | "";
      nav.edge = navJson["edge"] | "left";
      nav.thickness = navJson["thickness"] | 0;
      nav.entryLength = navJson["entryLength"] | 0;
      nav.backgroundColor = navJson["backgroundColor"] | "#ffffff";
      nav.backgroundColorDark = navJson["backgroundColorDark"] | "";
      nav.borderColor = navJson["borderColor"] | "";
      nav.borderColorDark = navJson["borderColorDark"] | "";
      if (navJson["themeSwitch"].is<JsonObject>()) {
        JsonObject sw = navJson["themeSwitch"].as<JsonObject>();
        nav.themeSwitchAt = sw["at"] | "";
        nav.themeSwitchLength = sw["length"] | 0;
        for (JsonObject o : sw["objects"].as<JsonArray>()) nav.themeSwitchObjects.push_back(parseScreenObject(o));
      }
      for (JsonObject entryJson : navJson["entries"].as<JsonArray>()) {
        NavigatorEntry entry;
        entry.screenId = entryJson["screenId"] | "";
        for (JsonObject o : entryJson["normal"].as<JsonArray>()) entry.normal.push_back(parseScreenObject(o));
        for (JsonObject o : entryJson["active"].as<JsonArray>()) entry.active.push_back(parseScreenObject(o));
        nav.entries.push_back(entry);
      }
      // The way back (2.10), by the way its arrow points.
      static const char* const ARROWS[4] = {"up", "down", "left", "right"};
      for (int e = 0; e < 4; e++) {
        for (JsonObject o : navJson["back"][ARROWS[e]].as<JsonArray>()) nav.back[e].push_back(parseScreenObject(o));
      }
      project_.navigators.push_back(nav);
    }
  }

  // The main row (contract 2.10).
  project_.mainRow.clear();
  for (JsonVariant id : doc["mainRow"].as<JsonArray>()) project_.mainRow.push_back(id.as<String>());

  // Parse hardware button defaults (project-wide, per physical button id)
  if (doc["hardwareButtons"].is<JsonArray>()) {
    parseHardwareButtons(doc["hardwareButtons"].as<JsonArray>());
  }

  // Combined topics (contract 2.6): the export writes them in evaluation
  // order; ordered again here, so a file edited by hand still computes right.
  // One with a circular reference is never exported; here it leaves none.
  project_.combinedTopics.clear();
  if (doc["combinedTopics"].is<JsonArray>()) {
    project_.combinedTopics = combined::evaluationOrder(combined::parse(doc["combinedTopics"].as<JsonArrayConst>()));
  }
  combinedDirty_ = true;

  return true;
}

void ProjectLoader::parseTopics(JsonArray topicsArray) {
  for (JsonObject topicJson : topicsArray) {
    MQTTTopic topic;
    topic.id = topicJson["id"] | "";
    topic.topic = topicJson["topic"] | "";
    topic.type = topicJson["type"] | "string";
    
    if (topicJson["examples"].is<JsonArray>()) {
      JsonArray examples = topicJson["examples"];
      for (JsonVariant ex : examples) {
        topic.examples.push_back(ex.as<String>());
      }
    }
    // currentValue stays empty until a real value arrives. Examples are for
    // designing a screen, not for showing it: a panel that started on the
    // first example showed a tank as full, or a relay as on, before it had
    // heard anything - and never said so. What each type draws for "no value"
    // is the designer's own table (docs/2026-09-15-live-data.md, decision 6);
    // conformance checks it before publishing anything to a fresh install.

    // Only present when type == "json" - absent for every other topic and
    // for any project.json predating this feature (additive field).
    if (topicJson["subtopics"].is<JsonArray>()) {
      for (JsonObject subJson : topicJson["subtopics"].as<JsonArray>()) {
        JsonSubtopic sub;
        sub.id = subJson["id"] | "";
        sub.path = subJson["path"] | "";
        sub.type = subJson["type"] | "text";
        topic.subtopics.push_back(sub);
      }
    }

    // A local topic starts at its initial value, as if it had arrived - at
    // boot and after every deploy, since both load the project (contract 2.9).
    topic.local = topicJson["local"] | false;
    if (topic.local && !topicJson["initial"].isNull()) {
      topic.initial = topicJson["initial"].as<String>();
      topic.currentValue = topic.initial;
      topic.arrived = true;
    }

    project_.topics.push_back(topic);
  }
}

bool ProjectLoader::isLocalTopic(const String& topicOrPath) const {
  const int hash = topicOrPath.indexOf('#');
  const String bare = hash >= 0 ? topicOrPath.substring(0, hash) : topicOrPath;
  for (const auto& t : project_.topics) {
    if (t.local && t.topic == bare) return true;
  }
  return false;
}

void ProjectLoader::parseFonts(JsonArray fontsArray) {
  project_.fonts.clear();
  // See parseScreens()'s identical reserve() calls for why - avoids
  // std::vector's doubling-reallocation strategy needing the old *and* new
  // buffer simultaneously mid-loop, on top of whatever the DynamicJsonDocument
  // this array lives in has already claimed.
  project_.fonts.reserve(fontsArray.size());

  for (JsonObject fontJson : fontsArray) {
    Font font;
    font.id = fontJson["id"] | "";
    font.name = fontJson["name"] | "";
    font.displayName = fontJson["displayName"] | "";
    font.internalName = fontJson["internalName"] | "";
    font.size = fontJson["size"] | 12;
    font.ascent = fontJson["ascent"] | 10;
    font.descent = fontJson["descent"] | 2;
    
    project_.fonts.push_back(font);
  }
}

void ProjectLoader::parseScreens(JsonArray screensArray) {
  // Root cause of a real boot crash found flashing real hardware
  // (2026-08-15): std::vector<ScreenObject>::push_back() below grows by
  // doubling, which needs the old block *and* a bigger new one allocated
  // simultaneously at the moment it reallocates - a real spike on top of
  // the DynamicJsonDocument this whole JsonArray already lives inside
  // (parseJSONFromFile's own bufferSize, itself already sized close to the
  // ESP.getMaxAllocHeap() ceiling that guard checks). ScreenObject is a
  // large element (five String fields plus a nested std::vector for
  // tab-control/panel children), so this wasn't a marginal effect -
  // observed crashing on the very project that guard was written to fix
  // (2026-08-10's HIL comprehensive-test project.json), well after that
  // guard had already passed. reserve()'d upfront, this becomes exactly
  // one correctly-sized allocation instead of an unknown number of
  // doubling reallocations, each briefly needing ~1.5x the vector's
  // eventual final size on top of everything else already resident.
  project_.screens.reserve(screensArray.size());

  for (JsonObject screenJson : screensArray) {
    Screen screen;
    parseScreenInto(screenJson, screen);
    project_.screens.push_back(screen);
  }
}

// Popups (designer docs/device-contract.md 2.5): a screen's shape and its
// frame colours, and the one fence of the project. A fence that is not there
// or has no size leaves popups unopenable rather than drawn wrong.
void ProjectLoader::parsePopups(JsonArray popupsArray, JsonObject fenceJson) {
  project_.popups.reserve(popupsArray.size());
  for (JsonObject popupJson : popupsArray) {
    Screen popup;
    parseScreenInto(popupJson, popup);
    popup.borderColor = popupJson["borderColor"] | "";
    popup.borderColorDark = popupJson["borderColorDark"] | "";
    popup.scrimColor = popupJson["scrimColor"] | "#000000";
    project_.popups.push_back(popup);
  }
  PopupFence& fence = project_.popupFence;
  if (!fenceJson.isNull()) {
    fence.circle = String(fenceJson["shape"] | "rect") == "circle";
    fence.x = fenceJson["x"] | 0;
    fence.y = fenceJson["y"] | 0;
    fence.width = fenceJson["width"] | 0;
    fence.height = fenceJson["height"] | 0;
    fence.present = fence.width > 0 && fence.height > 0;
  }
}

void ProjectLoader::parseScreenInto(JsonObject screenJson, Screen& screen) {
  {
    screen.id = screenJson["id"] | "";
    screen.name = screenJson["name"] | "";
    screen.path = resolveAssetPath(screenJson["path"] | "");
    screen.backgroundColor = screenJson["backgroundColor"] | "#ffffff";
    // No default for a dark field: empty means "the same as light", and the
    // light default is not a dark value (ThemeVariant.h).
    screen.backgroundColorDark = screenJson["backgroundColorDark"] | "";
    screen.pageIconPath = resolveAssetPath(screenJson["pageIconPath"] | "");
    screen.navigatorId = screenJson["navigatorId"] | "";
    screen.hidden = screenJson["hidden"] | false;
    // The screen graph (contract 2.10).
    static const char* const EDGES[4] = {"up", "down", "left", "right"};
    for (int e = 0; e < 4; e++) screen.neighbours[e] = screenJson["neighbours"][EDGES[e]] | "";
    screen.back = screenJson["back"] | "";
    screen.pullTabColor = screenJson["pullTabColor"] | "";
    screen.timeRange = screenJson["timeRange"] | "";
    screen.pullTabColorDark = screenJson["pullTabColorDark"] | "";

    if (screenJson["objects"].is<JsonArray>()) {
      JsonArray objectsArray = screenJson["objects"];
      screen.objects.reserve(objectsArray.size());
      for (JsonObject objJson : objectsArray) {
        ScreenObject obj = parseScreenObject(objJson);
        screen.objects.push_back(obj);
      }
    }

    // buttonActions is a JSON *object* keyed by button id (e.g. "button-0"),
    // not an array - mirrors the designer's Record<string,
    // HardwareButtonAction> (components/project-editor.tsx), which
    // JSON.stringify's the same way.
    if (screenJson["buttonActions"].is<JsonObject>()) {
      screen.buttonActions = parseScreenButtonActions(screenJson["buttonActions"].as<JsonObject>());
    }
  }
}

// Reads a JSON number that is meant to be a whole pixel, whatever the
// designer happened to write.
//
// `objJson["x"] | 0` cannot do this. ArduinoJson's `|` returns the fallback
// unless the stored value is already of the requested type, and a JSON float
// with a fractional part is not an integer - so 400.5 does not truncate to
// 400, it becomes 0. Silently, and only for the coordinates that happen to
// land off a whole pixel.
//
// The result is a shape with one vertex dragged into the screen corner, or an
// object drawn at the top left, which looks like a rendering bug and is not
// one. Found 2026-09-10 by the generated type-coverage run: its line specimen
// put the apex at width/2 of an odd width, the board drew the apex at x=0,
// and the picture was wrong in a way that took a pixel comparison to see and
// this comment to explain.
//
// Rounding rather than truncating, because a half pixel belongs to the nearer
// one and that is what the designer's own canvas does with it.
static int pixelValue(JsonVariantConst value, int fallback = 0) {
  if (!value.is<float>() && !value.is<int>()) return fallback;
  return (int)lroundf(value.as<float>());
}

ScreenObject ProjectLoader::parseScreenObject(JsonObject objJson) {
  ScreenObject obj;
  obj.type = objJson["type"] | "";
  obj.id = objJson["id"] | "";
  obj.x = pixelValue(objJson["x"]);
  obj.y = pixelValue(objJson["y"]);
  obj.width = pixelValue(objJson["width"]);
  obj.height = pixelValue(objJson["height"]);
  obj.zIndex = objJson["zIndex"] | 0;
  obj.path = resolveAssetPath(objJson["path"] | "");
  obj.pathDark = resolveAssetPath(objJson["pathDark"] | "");

  if (objJson["properties"].is<JsonObject>()) {
    obj.properties = parseObjectProperties(objJson["properties"]);
  }
  // A software button's look (the designer's device contract, «button»).
  if (obj.type == "button" && objJson["look"].is<JsonObject>()) {
    ButtonLookProps* b = new ButtonLookProps();
    auto face = [](JsonVariant j, ButtonFace& out) {
      if (!j.is<JsonObject>()) return;
      out.r = j["r"] | 0;
      out.fill = j["fill"] | "";
      out.outline = j["outline"] | "";
      out.ink = j["ink"] | "";
    };
    JsonObject look = objJson["look"];
    face(look["normal"], b->normal);
    face(look["pressed"], b->pressed);
    if (objJson["lookDark"].is<JsonObject>()) {
      face(objJson["lookDark"]["normal"], b->normalDark);
      face(objJson["lookDark"]["pressed"], b->pressedDark);
    }
    b->textX = look["textX"] | 0;
    b->baseline = look["baseline"] | 0;
    b->clipX = look["clipX"] | 0;
    b->clipW = look["clipW"] | 0;
    if (look["icon"].is<JsonObject>()) {
      b->hasIcon = true;
      b->iconX = look["icon"]["x"] | 0;
      b->iconY = look["icon"]["y"] | 0;
      b->iconSize = look["icon"]["size"] | 0;
    }
    b->iconPath = resolveAssetPath(objJson["iconPath"] | "");
    b->iconPathPressed = resolveAssetPath(objJson["iconPathPressed"] | "");
    b->iconPathDark = resolveAssetPath(objJson["iconPathDark"] | "");
    b->iconPathPressedDark = resolveAssetPath(objJson["iconPathPressedDark"] | "");
    obj.properties.button.p.reset(b);
  }
  // A Toggle Button's looks (contract «toggle-button»): one per state, none,
  // the ring; dark beside light, colours only.
  if (obj.type == "toggle-button" && objJson["looks"].is<JsonArray>()) {
    ToggleButtonProps* t = new ToggleButtonProps();
    auto face = [](JsonVariant j, ButtonFace& out) {
      if (!j.is<JsonObject>()) return;
      out.r = j["r"] | 0;
      out.fill = j["fill"] | "";
      out.outline = j["outline"] | "";
      out.ink = j["ink"] | "";
    };
    auto look = [&](JsonVariant j, JsonVariant dark, ToggleLook& out) {
      if (!j.is<JsonObject>()) return;
      face(j["normal"], out.normal);
      face(j["pressed"], out.pressed);
      out.label = j["label"] | "";
      out.textX = j["textX"] | 0;
      out.baseline = j["baseline"] | 0;
      out.clipX = j["clipX"] | 0;
      out.clipW = j["clipW"] | 0;
      if (j["icon"].is<JsonObject>()) {
        out.hasIcon = true;
        out.iconX = j["icon"]["x"] | 0;
        out.iconY = j["icon"]["y"] | 0;
        out.iconSize = j["icon"]["size"] | 0;
      }
      if (j["second"].is<JsonObject>()) {
        out.hasSecond = true;
        out.secondTextX = j["second"]["textX"] | 0;
        out.secondBaseline = j["second"]["baseline"] | 0;
        out.secondFontId = j["second"]["fontId"] | "";
      }
      if (j["disc"].is<JsonObject>()) {
        JsonVariant d = j["disc"];
        out.hasDisc = true;
        out.discCx = d["cx"] | 0;
        out.discCy = d["cy"] | 0;
        out.discR = d["r"] | 0;
        out.discNormal = d["normal"] | "";
        out.discPressed = d["pressed"] | "";
        if (d["edge"].is<JsonObject>()) {
          out.discEdgeNormal = d["edge"]["normal"] | "";
          out.discEdgePressed = d["edge"]["pressed"] | "";
        }
      }
      if (dark.is<JsonObject>()) {
        face(dark["normal"], out.normalDark);
        face(dark["pressed"], out.pressedDark);
        if (dark["disc"].is<JsonObject>()) {
          out.discNormalDark = dark["disc"]["normal"] | "";
          out.discPressedDark = dark["disc"]["pressed"] | "";
          if (dark["disc"]["edge"].is<JsonObject>()) {
            out.discEdgeNormalDark = dark["disc"]["edge"]["normal"] | "";
            out.discEdgePressedDark = dark["disc"]["edge"]["pressed"] | "";
          }
        }
      }
    };
    JsonArray looks = objJson["looks"].as<JsonArray>();
    JsonArray looksDark = objJson["looksDark"].as<JsonArray>();
    for (size_t i = 0; i < looks.size(); i++) {
      ToggleLook l;
      JsonVariant dark = looksDark.isNull() || i >= looksDark.size() ? JsonVariant() : JsonVariant(looksDark[i]);
      look(looks[i], dark, l);
      t->looks.push_back(l);
    }
    look(objJson["noneLook"], objJson["noneLookDark"], t->none);
    t->ring = objJson["ring"] | "";
    t->ringDark = objJson["ringDark"] | "";
    obj.properties.toggle.p.reset(t);
  }
  if (obj.type == "camera" && objJson["properties"].is<JsonObject>()) {
    CameraProps* c = new CameraProps();
    c->url = objJson["properties"]["url"] | "";
    obj.properties.camera.p.reset(c);
  }
  if (obj.type == "energy-diagram" && objJson["properties"].is<JsonObject>()) {
    obj.properties.energy.p.reset(new EnergyDiagramProps(parseEnergyDiagram(objJson["properties"])));
  }
  if (obj.type == "chart" && objJson["properties"].is<JsonObject>()) {
    obj.properties.chart.p.reset(new ChartProps(parseChart(objJson["properties"])));
  }
  // A text field without a read topic reads its write topic: it shows what
  // was last sent, as Home Assistant's optimistic text does (designer
  // render-text-field.ts textFieldValue).
  if ((obj.type == "text-field" || obj.type == "time-field") && obj.properties.topic.isEmpty()) obj.properties.topic = obj.properties.writeTopic;

  // Recursive: only "switcher" and "panel" objects carry children in
  // practice, but parsing is generic (matches the designer's ScreenObject
  // - any object type is technically allowed to have children). Absent for
  // every existing project (additive JSON field), so old projects parse
  // identically to before.
  if (objJson["children"].is<JsonArray>()) {
    JsonArray childrenArray = objJson["children"].as<JsonArray>();
    obj.children.reserve(childrenArray.size());
    for (JsonObject childJson : childrenArray) {
      obj.children.push_back(parseScreenObject(childJson));
    }
  }

  return obj;
}

ObjectProperties ProjectLoader::parseObjectProperties(JsonObject propsJson) {
  ObjectProperties props;
  
  // Common properties
  props.topic = propsJson["topic"] | "";
  props.displayAs = propsJson["displayAs"] | "";
  props.backgroundColor = propsJson["backgroundColor"] | "#ffffff";
  // Matches the designer's fallback default exactly (render-text-box.ts,
  // render-level-indicator.ts both default to "#cccccc" when unset) - this
  // used to default to "#000000" here, a real discrepancy that just never
  // got exercised because every HIL test project so far always set
  // borderColor explicitly (2026-07-21 finding, level indicator work).
  props.borderColor = propsJson["borderColor"] | "#cccccc";
  props.textColor = propsJson["textColor"] | "#000000";
  // Empty when the project does not carry one, NOT black.
  //
  // The designer reads this as `color || textColor || "#000000"`, where an
  // absent color is undefined and falls through. Defaulting it to black here
  // made it never empty, so the fallback to textColor was dead code and
  // every text box drew black - which looks right on a light background and
  // is invisible on a dark one. An MqttDataField asking for white on black
  // drew nothing at all, and nothing at all is indistinguishable from a
  // missing object, a missing font or a missing value. It cost an hour.
  //
  // The two callers that genuinely want black when nothing is set - lines -
  // now say so themselves, which is also what the designer does.
  props.color = propsJson["color"] | "";
  // The dark variant's colours, no defaults (ThemeVariant.h). iconColor and
  // buttonColor have none here because they have no light field either:
  // icons and buttons arrive baked, dark ones through the *Dark paths.
  props.backgroundColorDark = propsJson["backgroundColorDark"] | "";
  props.borderColorDark = propsJson["borderColorDark"] | "";
  props.textColorDark = propsJson["textColorDark"] | "";
  props.colorDark = propsJson["colorDark"] | "";
  props.fillColorDark = propsJson["fillColorDark"] | "";
  props.fillEndColorDark = propsJson["fillEndColorDark"] | "";
  props.trackColorDark = propsJson["trackColorDark"] | "";
  props.trackEdgeColorDark = propsJson["trackEdgeColorDark"] | "";
  props.strokeColorDark = propsJson["strokeColorDark"] | "";
  props.switchColorDark = propsJson["switchColorDark"] | "";
  props.fieldColor = propsJson["fieldColor"] | "";
  props.fieldColorDark = propsJson["fieldColorDark"] | "";
  props.lineColor = propsJson["lineColor"] | "";
  props.lineColorDark = propsJson["lineColorDark"] | "";
  props.minLength = propsJson["minLength"] | 0;
  props.maxLength = propsJson["maxLength"] | textfield::MAX_LENGTH;
  props.hideText = propsJson["hidden"] | false;
  props.keyboard = propsJson["keyboard"] | "";
  props.minuteStep = propsJson["minuteStep"] | 5;
  props.textAlign = propsJson["textAlign"] | "left";
  props.fontId = propsJson["fontId"] | "";
  props.fontWeight = propsJson["fontWeight"] | "normal";
  
  // Label properties
  props.text = propsJson["text"] | "";
  // Live values (contract 2.6). An icon result's path resolves like `path`.
  props.liveText = propsJson["liveText"] | "";
  props.liveIconId = propsJson["liveIconId"] | "";
  props.liveValues.clear();
  if (propsJson["liveValues"].is<JsonArray>()) {
    props.liveValues = livevalue::parseLiveValues(propsJson["liveValues"].as<JsonArrayConst>());
    auto resolve = [](livevalue::Result& result) {
      if (result.kind != livevalue::ResultKind::Icon) return;
      result.path = resolveAssetPath(String(result.path.c_str())).c_str();
      result.pathDark = resolveAssetPath(String(result.pathDark.c_str())).c_str();
    };
    for (auto& lv : props.liveValues) {
      for (auto& rule : lv.rules) resolve(rule.result);
      if (lv.otherwise) resolve(*lv.otherwise);
      if (lv.noValueYet) resolve(*lv.noValueYet);
    }
  }
  props.fontSize = propsJson["fontSize"] | 12;
  
  // MqttDataField properties
  props.prefix = propsJson["prefix"] | "";
  props.postfix = propsJson["postfix"] | "";
  props.thousandsSeparator = propsJson["thousandsSeparator"] | "";
  props.numberOfDecimals = propsJson["numberOfDecimals"] | 2;
  
  // MQTTIconField properties
  if (propsJson["valueIconPairs"].is<JsonArray>()) {
    props.valueIconPairs = parseValueIconPairs(propsJson["valueIconPairs"]);
  }
  
  // level-indicator properties
  //
  // `direction` and `thickness` are the names both shapes now use, since
  // 2026-09-22 - one thing, one name. The designer's property panel writes
  // them and stamps `thickness` on every new bar, while lib/project-zip.ts
  // exports properties verbatim; a device that read only the old names drew
  // every such bar at the default 16 px, left to right, no matter what the
  // author had set. The legacy names stay as the fallback, because the
  // projects in the van are full of them and nothing here rewrites a file
  // someone else owns. Same two-name reading as arcThickness below.
  props.barDirection = propsJson["direction"] | (propsJson["barDirection"] | "left-to-right");
  // Without one, the value, as the designer and the app show it
  // (lib/level-shape.ts's levelShowsNumber); "none" here left the number off
  // a bar the designer drew with one (designer #72).
  props.displayValue = propsJson["displayValue"] | "value";
  props.fillColor = propsJson["fillColor"] | "#4CAF50";
  props.fillEndColor = propsJson["fillEndColor"] | "";
  props.trackColor = propsJson["trackColor"] | "";
  props.trackEdgeColor = propsJson["trackEdgeColor"] | "";
  props.glow = propsJson["glow"] | 0;
  props.barThickness = propsJson["thickness"] | (propsJson["barThickness"] | 0);
  props.switchStyle = propsJson["switchStyle"] | "filled";
  props.switchColor = propsJson["switchColor"] | "";
  
  if (propsJson["calibrationPoints"].is<JsonArray>()) {
    props.calibrationPoints = parseCalibrationPoints(propsJson["calibrationPoints"]);
  }

  // arc-level properties. The defaults are the designer's own: half past
  // seven round to half past four, the thermostat shape.
  props.setpointTopic = propsJson["setpointTopic"] | "";
  props.markerColor = propsJson["markerColor"] | "#ffffff";
  props.arcDirection = propsJson["direction"] | "cw";
  props.minAngle = propsJson["minAngle"] | 225;
  props.maxAngle = propsJson["maxAngle"] | 135;
  // `thickness` first, `barThickness` behind it, and 0 meaning "the author set
  // none" - the renderer then draws the one default both shapes share
  // (levelshape::kDefaultThickness, 16 since 2026-09-22). It was 22 here while
  // a ring was thought to be a thing of its own, and that survived exactly as
  // long as it took to put a dial and a slider on one screen.
  props.arcThickness = propsJson["thickness"] | props.barThickness;
  // Retired on 2026-09-22 along with markerColor above: the
  // setpoint is the bar's own handle now, and its size follows the thickness.
  // Still parsed, because old project files carry it and nothing here rewrites
  // a file someone else owns - nothing reads it any more.
  props.markerWidth = propsJson["markerWidth"] | 4;

  // line properties
  props.strokeWidth = pixelValue(propsJson["strokeWidth"], 1);
  props.strokeStyle = propsJson["strokeStyle"] | "solid";
  props.filletRadius = pixelValue(propsJson["filletRadius"]);
  if (propsJson["points"].is<JsonArray>()) {
    props.points = parseLinePoints(propsJson["points"]);
  }
  props.arrowStart = propsJson["arrowStart"] | false;
  props.arrowEnd = propsJson["arrowEnd"] | false;

  // MqttDataLine properties - independent per-end arrow conditions, see
  // ProjectTypes.h's ObjectProperties comment for why these can't reuse
  // panel's single comparisonOperator/Value pair.
  props.arrowStartOperator = propsJson["arrowStartOperator"] | "<";
  props.arrowStartValue = propsJson["arrowStartValue"] | "0";
  props.arrowEndOperator = propsJson["arrowEndOperator"] | ">";
  props.arrowEndValue = propsJson["arrowEndValue"] | "0";

  // box properties - strokeColor defaults to empty (no border), matching
  // the designer's `if (strokeColor && strokeWidth > 0)` check exactly.
  props.strokeColor = propsJson["strokeColor"] | "";
  props.cornerRadius = pixelValue(propsJson["cornerRadius"]);
  // The shapes a free area clips it by (device contract 2.1, `clip`).
  if (propsJson["clip"].is<JsonArrayConst>()) {
    for (JsonObjectConst c : propsJson["clip"].as<JsonArrayConst>()) {
      ClipShape shape;
      shape.x = pixelValue(c["x"]);
      shape.y = pixelValue(c["y"]);
      shape.width = pixelValue(c["width"]);
      shape.height = pixelValue(c["height"]);
      shape.radius = pixelValue(c["radius"]);
      if (shape.width > 0 && shape.height > 0) props.clips.push_back(shape);
    }
  }

  // panel properties - matched against the parent tab-control's own
  // `topic` (already parsed above). Default operator "==" covers the
  // common enum-tab case (e.g. mode == "TEMP").
  props.comparisonOperator = propsJson["comparisonOperator"] | "==";
  props.comparisonValue = propsJson["comparisonValue"] | "";

  // SoftwareButton action - same ButtonAction shape/parser as hardware
  // buttons (see ProjectTypes.h's ButtonAction comment). Absent for every
  // other object type's properties (additive JSON field), and for a
  // SoftwareButton with no action configured - both leave props.action at
  // its default-constructed, empty-type value.
  if (propsJson["action"].is<JsonObject>()) {
    props.action = parseButtonAction(propsJson["action"].as<JsonObject>());
  }

  // Switch properties - see ProjectTypes.h's ObjectProperties comment.
  props.writeTopic = propsJson["writeTopic"] | "";
  props.step = propsJson["step"] | 1.0f;
  if (propsJson["writeFormat"].is<JsonObject>()) props.writeFormat = scaled::parseWriteFormat(propsJson["writeFormat"].as<JsonObjectConst>());
  props.markerStyle = propsJson["markerStyle"] | "";
  // No `mode` here any more: since 2026-09-20 the form is the object's own
  // type, "switch" or "button-group". A project still carrying the old key
  // is migrated by the designer before it is ever exported.
  props.activeBackgroundColor = propsJson["activeBackgroundColor"] | "#2563eb";
  if (propsJson["states"].is<JsonArray>()) {
    props.states = parseSwitchStates(propsJson["states"]);
  }

  return props;
}

std::vector<SwitchState> ProjectLoader::parseSwitchStates(JsonArray statesArray) {
  std::vector<SwitchState> states;

  for (JsonObject stateJson : statesArray) {
    SwitchState state;
    state.id = stateJson["id"] | "";
    state.label = stateJson["label"] | "";
    state.readValue = stateJson["readValue"] | "";
    state.writeValue = stateJson["writeValue"] | "";
    state.iconPath = resolveAssetPath(stateJson["path"] | "");
    state.iconPathActive = resolveAssetPath(stateJson["pathActive"] | "");
    state.iconPathDark = resolveAssetPath(stateJson["pathDark"] | "");
    state.iconPathActiveDark = resolveAssetPath(stateJson["pathActiveDark"] | "");
    // A Toggle Button's state icons (contract «toggle-button»).
    if (state.iconPath.isEmpty()) state.iconPath = resolveAssetPath(stateJson["iconPath"] | "");
    if (state.iconPathDark.isEmpty()) state.iconPathDark = resolveAssetPath(stateJson["iconPathDark"] | "");
    state.iconPathPressed = resolveAssetPath(stateJson["iconPathPressed"] | "");
    state.iconPathPressedDark = resolveAssetPath(stateJson["iconPathPressedDark"] | "");
    // "Show this state as switched on", which decides whether a switch
    // showing it is drawn in colour. Renamed from showMarker on 2026-09-20
    // (designer docs/2026-09-20-switch-look.md, decision 4); every project
    // written before it says the old name, so both are read.
    state.showMarker = stateJson["showAsOn"] | (stateJson["showMarker"] | false);
    states.push_back(state);
  }

  return states;
}

std::vector<ValueIconPair> ProjectLoader::parseValueIconPairs(JsonArray pairsArray) {
  std::vector<ValueIconPair> pairs;
  
  for (JsonObject pairJson : pairsArray) {
    ValueIconPair pair;
    pair.id = pairJson["id"] | "";
    pair.comparisonOperator = pairJson["comparisonOperator"] | ">";
    // The designer stores this as a JSON *string* ("00", "01", ...), not a
    // JSON number - ArduinoJson's `| defaultValue` only substitutes the
    // default when is<float>() is false, and a string value never
    // satisfies is<float>() regardless of its content, so
    // `pairJson["value"] | 0.0f` silently returned 0.0f for every pair
    // here, every time, no matter what the string actually said. .as<
    // String>().toFloat() stringifies either a JSON string or number
    // correctly first, then parses it the same permissive way
    // getIconPathForValue()'s own valueStr.toFloat() already does.
    pair.value = pairJson["value"].as<String>().toFloat();
    pair.thenShowIcon = pairJson["thenShowIcon"] | "";
    pair.path = resolveAssetPath(pairJson["path"] | "");
    pair.pathDark = resolveAssetPath(pairJson["pathDark"] | "");
    
    pairs.push_back(pair);
  }
  
  return pairs;
}

String ProjectLoader::getTopicValue(const String& topicOrPath) const {
  // topicOrPath may be a plain topic or a "<topic>#<path>" composite
  // referencing one field of a "json"-type topic's payload - splitting it
  // here (rather than requiring every caller to know about the "#"
  // convention) is what lets every existing caller of this function
  // (ScreenRenderer's condition evaluation, onMqttMessage's diff-check,
  // etc.) transparently support JSON subtopics with no changes of their
  // own. "#" can't appear in a real MQTT topic name (reserved wildcard
  // char in the spec), so it's a collision-free separator - the designer's
  // lib/json-path.ts splits the same way.
  // A combined topic (contract 2.6) reads as "true" / "false" through the
  // same door, so /api/topic-values and the renderer need nothing else.
  if (topicOrPath.startsWith("combined:")) {
    const auto value = getCombinedValue(topicOrPath.substring(9));
    return value ? String(value->c_str()) : String();
  }
  String realTopic = topicOrPath;
  String path = "";
  int hashIndex = topicOrPath.indexOf('#');
  if (hashIndex >= 0) {
    realTopic = topicOrPath.substring(0, hashIndex);
    path = topicOrPath.substring(hashIndex + 1);
  }

  for (const auto& t : project_.topics) {
    if (t.topic == realTopic) {
      if (path.isEmpty()) {
        return t.currentValue;
      }
      return extractJsonField(t.currentValue, path);
    }
  }
  return "";
}

void ProjectLoader::setTopicValue(const String& topic, const String& value) {
  for (auto& t : project_.topics) {
    if (t.topic == topic) {
      t.currentValue = value;
      t.arrived = true;
      combinedDirty_ = true;
      return;
    }
  }
}

std::optional<String> ProjectLoader::pressedMessage(const ButtonAction& action) const {
  const auto message = buttonvalues::pressedMessage(
      action.mqttMessage.c_str(), action.onlyWhen.c_str(), [this](const std::string& name) { return getCombinedValue(name.c_str()); },
      [this](const std::string& path) -> std::optional<std::string> {
        if (!hasTopicValue(path.c_str())) return std::nullopt;
        return std::string(getTopicValue(path.c_str()).c_str());
      });
  if (!message) return std::nullopt;
  return String(message->c_str());
}

std::optional<std::string> ProjectLoader::getCombinedValue(const String& name) const {
  if (combinedDirty_) {
    combinedValues_ = combined::compute(project_.combinedTopics, [this](const std::string& path) -> std::optional<std::string> {
      const String topic(path.c_str());
      if (!hasTopicValue(topic)) return std::nullopt;
      return std::string(getTopicValue(topic).c_str());
    });
    combinedDirty_ = false;
  }
  auto found = combinedValues_.find(name.c_str());
  return found == combinedValues_.end() ? std::nullopt : found->second;
}

bool ProjectLoader::hasTopicValue(const String& topicOrPath) const {
  if (topicOrPath.startsWith("combined:")) return getCombinedValue(topicOrPath.substring(9)).has_value();
  const int hashIndex = topicOrPath.indexOf('#');
  const String realTopic = hashIndex >= 0 ? topicOrPath.substring(0, hashIndex) : topicOrPath;
  for (const auto& t : project_.topics) {
    if (t.topic == realTopic) return t.arrived;
  }
  return false;
}

namespace {

// One JSONPath shorthand segment - either a member-name access (from
// ".name", a bare leading "name", or a quoted "['name']"/"[\"name\"]") or
// an array index (from "[N]"). Mirrors lib/json-path.ts's PathSegment
// exactly - see that file's header comment for the full syntax this
// supports and why it's a genuine JSONPath subset rather than an invented
// look-alike.
struct JsonPathSegment {
  bool isIndex;
  String name;
  int index;
};

std::vector<JsonPathSegment> tokenizeJsonPath(const String& path) {
  String s = path;
  s.trim();
  if (s.startsWith("$")) {
    s = s.substring(1);
  }

  std::vector<JsonPathSegment> segments;
  int i = 0;
  int len = s.length();

  while (i < len) {
    if (s[i] == '.') {
      i++;
      String name = "";
      while (i < len && s[i] != '.' && s[i] != '[') {
        name += s[i];
        i++;
      }
      if (!name.isEmpty()) {
        segments.push_back({false, name, 0});
      }
    } else if (s[i] == '[') {
      int closeIndex = s.indexOf(']', i);
      if (closeIndex == -1) break;  // malformed - resolve what was parsed so far
      String inner = s.substring(i + 1, closeIndex);
      inner.trim();
      bool quoted = (inner.startsWith("'") && inner.endsWith("'")) || (inner.startsWith("\"") && inner.endsWith("\""));
      if (quoted) {
        segments.push_back({false, inner.substring(1, inner.length() - 1), 0});
      } else {
        segments.push_back({true, "", inner.toInt()});
      }
      i = closeIndex + 1;
    } else {
      // Bare leading name with no "." or "$" prefix - the ergonomic
      // shorthand this app's paths have always used ("temp" instead of
      // requiring "$.temp" or ".temp").
      String name = "";
      while (i < len && s[i] != '.' && s[i] != '[') {
        name += s[i];
        i++;
      }
      if (!name.isEmpty()) {
        segments.push_back({false, name, 0});
      }
    }
  }

  return segments;
}

}  // namespace

String ProjectLoader::extractJsonField(const String& jsonPayload, const String& path) const {
  if (path.isEmpty()) return jsonPayload;

  // Small, transient document - this runs per MQTT message on a payload
  // capped at MQTT_MAX_PACKET_SIZE (see MqttClient::connect()), not the
  // large one-time project.json parse that needs a pre-sized
  // DynamicJsonDocument.
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, jsonPayload);
  if (error) return "";

  JsonVariant current = doc.as<JsonVariant>();

  for (const auto& segment : tokenizeJsonPath(path)) {
    if (current.isNull()) return "";
    if (segment.isIndex) {
      if (!current.is<JsonArray>()) return "";
      current = current[segment.index];
    } else {
      if (!current.is<JsonObject>()) return "";
      current = current[segment.name];
    }
  }

  if (current.isNull()) return "";

  // A path that stops partway through a nested structure (object/array)
  // gets JSON-stringified rather than yielding ArduinoJson's own
  // unhelpful default conversion - matches lib/json-path.ts's
  // extractJsonField on the designer side exactly, so a value that looks
  // right in the designer's preview is what this actually returns too.
  if (current.is<JsonObjectConst>() || current.is<JsonArrayConst>()) {
    String out;
    serializeJson(current, out);
    return out;
  }
  return current.as<String>();
}

std::vector<CalibrationPoint> ProjectLoader::parseCalibrationPoints(JsonArray pointsArray) {
  std::vector<CalibrationPoint> points;

  for (JsonObject pointJson : pointsArray) {
    CalibrationPoint point;
    point.value = pointJson["value"] | 0.0f;
    point.barSizePercent = pointJson["barSizePercent"] | 0.0f;

    points.push_back(point);
  }

  return points;
}

std::vector<Point> ProjectLoader::parseLinePoints(JsonArray pointsArray) {
  std::vector<Point> points;

  for (JsonObject pointJson : pointsArray) {
    Point point;
    // Rounded, not truncated, and emphatically not `| 0` - see pixelValue().
    // A line's own vertices are the field where a fractional coordinate is
    // most likely (a midpoint of an odd width) and where reading one as 0
    // does the most visible damage.
    point.x = pixelValue(pointJson["x"]);
    point.y = pixelValue(pointJson["y"]);

    points.push_back(point);
  }

  return points;
}

// The energy diagram's data (contract 2.8). Colours carry no default here:
// the export always writes them.
EnergyDiagramProps ProjectLoader::parseEnergyDiagram(JsonObject p) {
  EnergyDiagramProps e;
  e.sankey = String(p["mode"] | "flow") == "sankey";
  e.motion = p["motion"] | true;
  e.fullScale = p["fullScale"] | 1000.0f;
  if (!(e.fullScale > 0)) e.fullScale = 1000;
  e.fontId = p["fontId"] | "";
  e.valueFontId = p["valueFontId"] | e.fontId.c_str();
  e.fontSize = p["fontSize"] | 16;
  e.valueFontSize = p["valueFontSize"] | e.fontSize;
  e.textColor = p["textColor"] | "#1d1b20";
  e.textColorDark = p["textColorDark"] | "";
  e.mutedColor = p["mutedColor"] | "#49454f";
  e.mutedColorDark = p["mutedColorDark"] | "";
  e.busColor = p["busColor"] | "#6750a4";
  e.busColorDark = p["busColorDark"] | "";
  e.idleColor = p["idleColor"] | "#cac4d0";
  e.idleColorDark = p["idleColorDark"] | "";
  e.chargeColor = p["chargeColor"] | "#2f9a52";
  e.chargeColorDark = p["chargeColorDark"] | "";
  e.dischargeColor = p["dischargeColor"] | "#d0453f";
  e.dischargeColorDark = p["dischargeColorDark"] | "";
  if (!p["nodes"].is<JsonArray>()) return e;
  for (JsonObject nj : p["nodes"].as<JsonArray>()) {
    EnergyNodeProps n;
    n.id = nj["id"] | "";
    n.role = nj["role"] | "rest";
    n.name = nj["name"] | "";
    n.x = nj["x"] | 0.0f;
    n.y = nj["y"] | 0.0f;
    n.topic = nj["topic"] | "";
    n.chargeTopic = nj["chargeTopic"] | "";
    n.reverseSign = nj["reverseSign"] | false;
    n.color = nj["color"] | "";
    n.colorDark = nj["colorDark"] | "";
    if (nj["icon"].is<JsonObject>()) {
      JsonObject ij = nj["icon"];
      n.hasIcon = true;
      n.icon.size = ij["size"] | 0;
      n.icon.live = resolveAssetPath(ij["live"] | "");
      n.icon.liveDark = resolveAssetPath(ij["liveDark"] | "");
      n.icon.charge = resolveAssetPath(ij["charge"] | "");
      n.icon.chargeDark = resolveAssetPath(ij["chargeDark"] | "");
      n.icon.discharge = resolveAssetPath(ij["discharge"] | "");
      n.icon.dischargeDark = resolveAssetPath(ij["dischargeDark"] | "");
      n.icon.muted = resolveAssetPath(ij["muted"] | "");
      n.icon.mutedDark = resolveAssetPath(ij["mutedDark"] | "");
    }
    if (nj["lines"].is<JsonArray>()) {
      for (JsonObject lj : nj["lines"].as<JsonArray>()) {
        if (n.lines.size() >= 3) break;
        EnergyLine line;
        line.text = lj["text"] | "";
        line.liveText = lj["liveText"] | "";
        if (lj["liveValues"].is<JsonArray>()) line.liveValues = livevalue::parseLiveValues(lj["liveValues"].as<JsonArrayConst>());
        n.lines.push_back(line);
      }
    }
    if (nj["action"].is<JsonObject>()) n.action = parseButtonAction(nj["action"].as<JsonObject>());
    e.nodes.push_back(n);
  }
  return e;
}

// A chart (contract 2.13): the core's model, and its colours apart for the
// dark swap.
ChartProps ProjectLoader::parseChart(JsonObject p) {
  ChartProps c;
  c.chart = chartcore::chartFrom(p);
  c.fontId = p["fontId"] | "";
  c.dateFontId = p["dateFontId"] | "";
  c.mutedColor = p["mutedColor"] | "";
  c.mutedColorDark = p["mutedColorDark"] | "";
  c.nowColor = p["nowColor"] | "";
  c.nowColorDark = p["nowColorDark"] | "";
  for (JsonObject s : p["series"].as<JsonArray>()) {
    c.seriesColor.push_back(s["color"] | "");
    c.seriesColorDark.push_back(s["colorDark"] | "");
  }
  for (JsonObject z : p["zones"].as<JsonArray>()) {
    c.zoneColor.push_back(z["color"] | "");
    c.zoneColorDark.push_back(z["colorDark"] | "");
  }
  return c;
}

ButtonAction ProjectLoader::parseButtonAction(JsonObject actionJson) {
  ButtonAction action;
  action.type = actionJson["type"] | "";
  action.targetScreenId = actionJson["targetScreenId"] | "";
  action.mqttTopic = actionJson["mqttTopic"] | "";
  action.mqttMessage = actionJson["mqttMessage"] | "";
  action.deviceActionId = actionJson["deviceActionId"] | "";
  action.targetObjectId = actionJson["targetObjectId"] | "";
  action.direction = actionJson["direction"] | "";
  action.side = actionJson["side"] | "";
  action.rangeDirection = actionJson["rangeDirection"] | "";
  action.range = actionJson["range"] | "";
  action.onlyWhen = actionJson["onlyWhen"] | "";
  action.clears = actionJson["clears"] | "";
  return action;
}

std::vector<ScreenButtonAction> ProjectLoader::parseScreenButtonActions(JsonObject buttonActionsJson) {
  std::vector<ScreenButtonAction> result;

  for (JsonPair kv : buttonActionsJson) {
    ScreenButtonAction sba;
    sba.buttonId = kv.key().c_str();
    if (kv.value().is<JsonObject>()) {
      sba.action = parseButtonAction(kv.value().as<JsonObject>());
    }
    result.push_back(sba);
  }

  return result;
}

void ProjectLoader::parseHardwareButtons(JsonArray hardwareButtonsArray) {
  for (JsonObject btnJson : hardwareButtonsArray) {
    HardwareButtonDef def;
    def.id = btnJson["id"] | "";
    if (btnJson["defaultAction"].is<JsonObject>()) {
      def.defaultAction = parseButtonAction(btnJson["defaultAction"].as<JsonObject>());
    }
    project_.hardwareButtons.push_back(def);
  }
}

void ProjectLoader::applyThemeVariant(bool dark) {
  themevariant::apply(project_, dark);
}

ButtonAction ProjectLoader::getButtonAction(int screenIndex, const String& buttonId) const {
  if (const Screen* screen = screenOrPopupAt(project_, screenIndex)) {
    for (const auto& sba : screen->buttonActions) {
      if (sba.buttonId == buttonId) {
        return sba.action;
      }
    }
  }
  // An open popup's own actions are all there is (designer
  // docs/device-contract.md 2.5): no project-wide default behind them.
  if (isPopupIndex(project_, screenIndex)) return ButtonAction();

  for (const auto& def : project_.hardwareButtons) {
    if (def.id == buttonId) {
      return def.defaultAction;
    }
  }

  return ButtonAction();
}

