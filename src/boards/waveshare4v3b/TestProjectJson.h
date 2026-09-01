#pragma once

// A hand-written project for the first render on this board.
//
// Boxes and lines only, deliberately: text would pull in the DDF font store,
// and the DDF for this panel is its own piece of work - the existing font set
// stops at helvR24, which is 4.1mm of line height at 0.117mm per pixel and
// unreadable across a vehicle. Keeping fonts out of this step means a wrong
// pixel here can only be the renderer, the canvas or the panel.
//
// PROPERTY NAMES MATTER, and the first version of this file got them wrong:
// a box reads fillColor / strokeColor / strokeWidth, NOT backgroundColor /
// borderColor / borderWidth. backgroundColor is the text-box background of a
// label or MqttDataField, a different thing - ColorScreenRenderer::renderBox
// carries a comment about that exact confusion having been a real bug on
// 2026-08-10. The failure was quietly total rather than obvious: with no
// fillColor set, every box took parseObjectProperties' default of #4CAF50,
// so the full-screen box painted the whole panel green and the three boxes
// on top of it painted green on green.
//
// Geometry is chosen so mistakes are visible rather than plausible: objects
// touch all four edges, overlap, and use colours that differ per channel.
static const char TEST_PROJECT_JSON[] = R"JSON({
  "name": "4v3b render smoke",
  "screenWidth": 800,
  "screenHeight": 480,
  "exportColorDepth": "24bit",
  "screens": [
    {
      "id": "screen-1",
      "name": "Render smoke",
      "backgroundColor": "#101820",
      "objects": [
        { "id": "edge", "type": "box", "x": 0, "y": 0, "width": 800, "height": 480,
          "zIndex": 0,
          "properties": { "fillColor": "transparent", "strokeColor": "#ffffff", "strokeWidth": 2 } },

        { "id": "red", "type": "box", "x": 40, "y": 40, "width": 300, "height": 160,
          "zIndex": 1,
          "properties": { "fillColor": "#c02828", "strokeColor": "#ffffff", "strokeWidth": 3 } },

        { "id": "green", "type": "box", "x": 200, "y": 120, "width": 300, "height": 160,
          "zIndex": 2,
          "properties": { "fillColor": "#28a028", "strokeColor": "#000000", "strokeWidth": 3 } },

        { "id": "blue", "type": "box", "x": 460, "y": 40, "width": 300, "height": 160,
          "zIndex": 1,
          "properties": { "fillColor": "#2850c8", "strokeColor": "#ffffff", "strokeWidth": 3 } },

        { "id": "diag", "type": "line", "x": 40, "y": 300, "width": 720, "height": 140,
          "zIndex": 3,
          "properties": { "color": "#ffd000", "strokeWidth": 4 } },

        { "id": "flat", "type": "line", "x": 40, "y": 440, "width": 720, "height": 0,
          "zIndex": 3,
          "properties": { "color": "#00d0d0", "strokeWidth": 2 } }
      ]
    },
    {
      "id": "screen-2",
      "name": "Real line geometry",
      "backgroundColor": "#101820",
      "objects": [
        { "id": "l01", "type": "line", "x": 20, "y": 100, "width": 20, "height": 260, "zIndex": 1, "properties": { "color": "#ffd000", "strokeWidth": 1 } },
        { "id": "l02", "type": "line", "x": 55, "y": 100, "width": 20, "height": 260, "zIndex": 1, "properties": { "color": "#ffd000", "strokeWidth": 1 } },
        { "id": "l03", "type": "line", "x": 90, "y": 100, "width": 20, "height": 260, "zIndex": 1, "properties": { "color": "#ffd000", "strokeWidth": 1 } },
        { "id": "l04", "type": "line", "x": 125, "y": 100, "width": 20, "height": 260, "zIndex": 1, "properties": { "color": "#ffd000", "strokeWidth": 1 } },
        { "id": "l05", "type": "line", "x": 160, "y": 100, "width": 20, "height": 260, "zIndex": 1, "properties": { "color": "#ffd000", "strokeWidth": 1 } },
        { "id": "l06", "type": "line", "x": 195, "y": 100, "width": 20, "height": 260, "zIndex": 1, "properties": { "color": "#ffd000", "strokeWidth": 1 } },
        { "id": "l07", "type": "line", "x": 230, "y": 100, "width": 20, "height": 260, "zIndex": 1, "properties": { "color": "#ffd000", "strokeWidth": 1 } },
        { "id": "l08", "type": "line", "x": 265, "y": 100, "width": 20, "height": 260, "zIndex": 1, "properties": { "color": "#ffd000", "strokeWidth": 1 } },
        { "id": "l09", "type": "line", "x": 300, "y": 100, "width": 20, "height": 260, "zIndex": 1, "properties": { "color": "#ffd000", "strokeWidth": 1 } },
        { "id": "l10", "type": "line", "x": 335, "y": 100, "width": 20, "height": 260, "zIndex": 1, "properties": { "color": "#ffd000", "strokeWidth": 1 } },
        { "id": "l11", "type": "line", "x": 370, "y": 100, "width": 20, "height": 260, "zIndex": 1, "properties": { "color": "#00d0d0", "strokeWidth": 1 } },
        { "id": "l12", "type": "line", "x": 405, "y": 100, "width": 20, "height": 260, "zIndex": 1, "properties": { "color": "#00d0d0", "strokeWidth": 1 } },
        { "id": "l13", "type": "line", "x": 440, "y": 100, "width": 20, "height": 260, "zIndex": 1, "properties": { "color": "#00d0d0", "strokeWidth": 1 } },
        { "id": "l14", "type": "line", "x": 475, "y": 100, "width": 20, "height": 260, "zIndex": 1, "properties": { "color": "#00d0d0", "strokeWidth": 1 } },
        { "id": "l15", "type": "line", "x": 510, "y": 100, "width": 20, "height": 260, "zIndex": 1, "properties": { "color": "#00d0d0", "strokeWidth": 1 } },
        { "id": "l16", "type": "line", "x": 545, "y": 100, "width": 20, "height": 260, "zIndex": 1, "properties": { "color": "#00d0d0", "strokeWidth": 1 } },
        { "id": "l17", "type": "line", "x": 580, "y": 100, "width": 20, "height": 260, "zIndex": 1, "properties": { "color": "#00d0d0", "strokeWidth": 1 } },
        { "id": "l18", "type": "line", "x": 615, "y": 100, "width": 20, "height": 260, "zIndex": 1, "properties": { "color": "#00d0d0", "strokeWidth": 1 } },
        { "id": "l19", "type": "line", "x": 650, "y": 100, "width": 20, "height": 260, "zIndex": 1, "properties": { "color": "#00d0d0", "strokeWidth": 1 } },
        { "id": "l20", "type": "line", "x": 685, "y": 100, "width": 20, "height": 260, "zIndex": 1, "properties": { "color": "#00d0d0", "strokeWidth": 1 } }
      ]
    }
  ]
})JSON";
