#pragma once
// A needle description as the export writes it (device contract §2.15
// `instrument.needle`), read into needleraster::Description. Header-only so
// the loader and the PC test read it alike.

#include <ArduinoJson.h>

#include "NeedleRaster.h"

namespace needleraster {

inline bool parseDescription(JsonObjectConst json, Description& out) {
  if (json.isNull()) return false;
  out.pivotX = json["pivot"]["x"] | 0;
  out.pivotY = json["pivot"]["y"] | 0;
  out.layers.clear();
  for (JsonObjectConst l : json["layers"].as<JsonArrayConst>()) {
    Layer layer;
    const char* motion = l["motion"] | "static";
    layer.motion = strcmp(motion, "rotate") == 0 ? Motion::Rotate : strcmp(motion, "translate") == 0 ? Motion::Translate : Motion::Static;
    layer.color = (uint16_t)(l["color"] | 0);
    layer.alpha = l["alpha"] | 256;
    layer.dx = l["dx"] | 0;
    layer.dy = l["dy"] | 0;
    for (JsonVariantConst a : l["soft"].as<JsonArrayConst>()) layer.soft.push_back(a.as<int32_t>());
    for (JsonObjectConst s : l["shapes"].as<JsonArrayConst>()) {
      Shape shape;
      if (s["circle"].is<JsonArrayConst>()) {
        JsonArrayConst c = s["circle"].as<JsonArrayConst>();
        shape.circle = true;
        shape.cx = c[0] | 0;
        shape.cy = c[1] | 0;
        shape.r = c[2] | 0;
        shape.rIn = c[3] | 0;
      } else {
        for (JsonArrayConst pt : s["poly"].as<JsonArrayConst>()) {
          PolygonRaster::Point p;
          p.x = pt[0] | 0;
          p.y = pt[1] | 0;
          shape.poly.push_back(p);
        }
      }
      layer.shapes.push_back(shape);
    }
    out.layers.push_back(layer);
  }
  return true;
}

}  // namespace needleraster
