#pragma once

// The built-in fallback project for the 4", shown when nothing has been
// deployed. The 4.3B's (waveshare4v3b/TestProjectJson.h, which explains the
// choices) laid out for 480 x 480: boxes and lines only, touching all four
// edges, overlapping, in colours that differ per channel - so a wrong pin,
// a swapped channel or an offset on the first boot shows rather than passes.
static const char TEST_PROJECT_JSON[] = R"JSON({
  "name": "ws4 render smoke",
  "screenWidth": 480,
  "screenHeight": 480,
  "exportColorDepth": "24bit",
  "screens": [
    {
      "id": "screen-1",
      "name": "Render smoke",
      "backgroundColor": "#101820",
      "objects": [
        { "id": "edge", "type": "box", "x": 0, "y": 0, "width": 480, "height": 480,
          "zIndex": 0,
          "properties": { "fillColor": "transparent", "strokeColor": "#ffffff", "strokeWidth": 2 } },

        { "id": "red", "type": "box", "x": 30, "y": 30, "width": 180, "height": 140,
          "zIndex": 1,
          "properties": { "fillColor": "#c02828", "strokeColor": "#ffffff", "strokeWidth": 3 } },

        { "id": "green", "type": "box", "x": 150, "y": 110, "width": 180, "height": 140,
          "zIndex": 2,
          "properties": { "fillColor": "#28a028", "strokeColor": "#000000", "strokeWidth": 3 } },

        { "id": "blue", "type": "box", "x": 270, "y": 30, "width": 180, "height": 140,
          "zIndex": 1,
          "properties": { "fillColor": "#2850c8", "strokeColor": "#ffffff", "strokeWidth": 3 } },

        { "id": "diag", "type": "line", "x": 30, "y": 290, "width": 420, "height": 120,
          "zIndex": 3,
          "properties": { "color": "#ffd000", "strokeWidth": 4 } },

        { "id": "flat", "type": "line", "x": 30, "y": 440, "width": 420, "height": 0,
          "zIndex": 3,
          "properties": { "color": "#00d0d0", "strokeWidth": 2 } }
      ]
    }
  ]
})JSON";
