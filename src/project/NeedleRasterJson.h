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
    layer.motion = strcmp(motion, "rotate") == 0      ? Motion::Rotate
                   : strcmp(motion, "translate") == 0 ? Motion::Translate
                   : strcmp(motion, "fill") == 0      ? Motion::Fill
                   : strcmp(motion, "segments") == 0  ? Motion::Segments
                                                      : Motion::Static;
    const char* grow = l["grow"] | "right";
    layer.grow = strcmp(grow, "left") == 0 ? Grow::Left : strcmp(grow, "down") == 0 ? Grow::Down : strcmp(grow, "up") == 0 ? Grow::Up : Grow::Right;
    JsonObjectConst g = l["gradient"].as<JsonObjectConst>();
    if (!g.isNull()) {
      layer.gradient.on = true;
      layer.gradient.to = (uint16_t)(g["to"] | 0);
      layer.gradient.alongY = strcmp(g["axis"] | "x", "y") == 0;
      layer.gradient.start = g["start"] | 0;
      const int32_t length = g["length"] | 1;
      layer.gradient.length = length < 1 ? 1 : length;
    }
    layer.color = (uint16_t)(l["color"] | 0);
    layer.alpha = l["alpha"] | 256;
    layer.dx = l["dx"] | 0;
    layer.dy = l["dy"] | 0;
    for (JsonVariantConst a : l["soft"].as<JsonArrayConst>()) layer.soft.push_back(a.as<int32_t>());
    for (JsonObjectConst s : l["shapes"].as<JsonArrayConst>()) {
      Shape shape;
      shape.t = s["t"].is<int32_t>() ? s["t"].as<int32_t>() : -1;
      if (s["circle"].is<JsonArrayConst>()) {
        JsonArrayConst c = s["circle"].as<JsonArrayConst>();
        shape.kind = Kind::Circle;
        shape.cx = c[0] | 0;
        shape.cy = c[1] | 0;
        shape.r = c[2] | 0;
        shape.rIn = c[3] | 0;
      } else if (s["rect"].is<JsonArrayConst>()) {
        JsonArrayConst r = s["rect"].as<JsonArrayConst>();
        shape.kind = Kind::Rect;
        shape.x0 = r[0] | 0;
        shape.y0 = r[1] | 0;
        shape.x1 = r[2] | 0;
        shape.y1 = r[3] | 0;
      } else if (s["arc"].is<JsonArrayConst>()) {
        JsonArrayConst a = s["arc"].as<JsonArrayConst>();
        shape.kind = Kind::Arc;
        shape.r = a[0] | 0;
        shape.hw = a[1] | 0;
        shape.from64 = a[2] | 0;
        shape.to64 = a[3] | 0;
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
