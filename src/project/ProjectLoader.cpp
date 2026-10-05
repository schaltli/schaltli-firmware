#include "ProjectLoader.h"
#include "ThemeVariant.h"
#include <exception>
#include <math.h>

namespace {
// Every asset path stored in project.json (icon `path`, SoftwareButton
// `pathNormal`/`pathActive`, MQTTIconField `valueIconPairs[].path`, a
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
  
  // Parse screens
  if (doc["screens"].is<JsonArray>()) {
    parseScreens(doc["screens"].as<JsonArray>());
  }

  // Parse hardware button defaults (project-wide, per physical button id)
  if (doc["hardwareButtons"].is<JsonArray>()) {
    parseHardwareButtons(doc["hardwareButtons"].as<JsonArray>());
  }

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

    project_.topics.push_back(topic);
  }
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
    screen.id = screenJson["id"] | "";
    screen.name = screenJson["name"] | "";
    screen.path = resolveAssetPath(screenJson["path"] | "");
    screen.backgroundColor = screenJson["backgroundColor"] | "#ffffff";
    // No default for a dark field: empty means "the same as light", and the
    // light default is not a dark value (ThemeVariant.h).
    screen.backgroundColorDark = screenJson["backgroundColorDark"] | "";
    screen.pageIconPath = resolveAssetPath(screenJson["pageIconPath"] | "");

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

    project_.screens.push_back(screen);
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
  obj.pathNormal = resolveAssetPath(objJson["pathNormal"] | "");
  obj.pathActive = resolveAssetPath(objJson["pathActive"] | "");
  obj.pathDark = resolveAssetPath(objJson["pathDark"] | "");
  obj.pathNormalDark = resolveAssetPath(objJson["pathNormalDark"] | "");
  obj.pathActiveDark = resolveAssetPath(objJson["pathActiveDark"] | "");

  if (objJson["properties"].is<JsonObject>()) {
    obj.properties = parseObjectProperties(objJson["properties"]);
  }

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
  props.textAlign = propsJson["textAlign"] | "left";
  props.fontId = propsJson["fontId"] | "";
  props.fontWeight = propsJson["fontWeight"] | "normal";
  
  // Label properties
  props.text = propsJson["text"] | "";
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
  props.displayValue = propsJson["displayValue"] | "none";
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
      return;
    }
  }
}

bool ProjectLoader::hasTopicValue(const String& topicOrPath) const {
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

ButtonAction ProjectLoader::parseButtonAction(JsonObject actionJson) {
  ButtonAction action;
  action.type = actionJson["type"] | "";
  action.targetScreenId = actionJson["targetScreenId"] | "";
  action.mqttTopic = actionJson["mqttTopic"] | "";
  action.mqttMessage = actionJson["mqttMessage"] | "";
  action.deviceActionId = actionJson["deviceActionId"] | "";
  action.targetObjectId = actionJson["targetObjectId"] | "";
  action.direction = actionJson["direction"] | "";
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
  if (screenIndex >= 0 && screenIndex < (int)project_.screens.size()) {
    const Screen& screen = project_.screens[screenIndex];
    for (const auto& sba : screen.buttonActions) {
      if (sba.buttonId == buttonId) {
        return sba.action;
      }
    }
  }

  for (const auto& def : project_.hardwareButtons) {
    if (def.id == buttonId) {
      return def.defaultAction;
    }
  }

  return ButtonAction();
}

