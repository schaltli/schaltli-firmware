// A Toggle Button's tap (designer contract «toggle-button»): which state
// switchshape::stateIndexForTap sends, printed one per line for
// tools/switch-shape/run.js to hold to the designer's toggleNextIndex
// (lib/toggle-button-shape.ts, e2e/toggle-button-shape.spec.ts «a tap»).
#include <cstdio>

#include "LevelShape.h"
#include "ProjectTypes.h"
#include "SwitchShape.h"

static ScreenObject tile(int count, int onIndex) {
  ScreenObject obj;
  obj.type = "toggle-button";
  obj.width = 216;
  obj.height = 84;
  for (int i = 0; i < count; i++) {
    SwitchState s;
    s.showMarker = i == onIndex;
    obj.properties.states.push_back(s);
  }
  return obj;
}

int main() {
  // Off, On (on), any x: the next one, wrapping.
  printf("%d\n", switchshape::stateIndexForTap(tile(2, 1), 10, 0));
  printf("%d\n", switchshape::stateIndexForTap(tile(2, 1), 200, 1));
  // Nothing reported: the first that shows as on, else the first.
  printf("%d\n", switchshape::stateIndexForTap(tile(2, 1), 10, -1));
  printf("%d\n", switchshape::stateIndexForTap(tile(2, -1), 10, -1));
  // Three states.
  printf("%d\n", switchshape::stateIndexForTap(tile(3, 1), 10, 1));
  printf("%d\n", switchshape::stateIndexForTap(tile(3, 1), 10, 2));
  // No states.
  printf("%d\n", switchshape::stateIndexForTap(tile(0, -1), 10, -1));
  return 0;
}
