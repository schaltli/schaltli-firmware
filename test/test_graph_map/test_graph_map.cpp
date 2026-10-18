// The knob's map of the screen graph (src/project/GraphMap), on the PC:
//
//   pio test -e native

#include <unity.h>

#include "project/GraphMap.h"

using namespace graphmap;

void setUp() {}
void tearDown() {}

// A, B, C in the row; A1 below A, C0 above C.
static std::vector<Tile> comb() { return layout({{"a", "", "a1"}, {"b", "", ""}, {"c", "c0", ""}}); }

static void the_row_across_and_what_lies_above_and_below_its_pages() {
    const std::vector<Tile> t = comb();
    TEST_ASSERT_EQUAL(5, (int)t.size());
    const Tile& a1 = t[indexOf(t, "a1")];
    TEST_ASSERT_EQUAL(0, a1.col);
    TEST_ASSERT_EQUAL(1, a1.level);
    const Tile& c0 = t[indexOf(t, "c0")];
    TEST_ASSERT_EQUAL(2, c0.col);
    TEST_ASSERT_EQUAL(-1, c0.level);
    TEST_ASSERT_EQUAL(-1, indexOf(t, "nowhere"));
}

static void the_current_screen_lies_in_the_middle() {
    const std::vector<Tile> t = comb();
    const int b = indexOf(t, "b");
    const Point centre = centreOf(t[b], t[b], 360, {0, 0});
    TEST_ASSERT_EQUAL(180, centre.x);
    TEST_ASSERT_EQUAL(180, centre.y);
    const Point a1 = centreOf(t[indexOf(t, "a1")], t[b], 360, {0, 0});
    TEST_ASSERT_EQUAL(180 - COL_STEP, a1.x);
    TEST_ASSERT_EQUAL(180 + ROW_STEP, a1.y);
    // From the history, its page lies above.
    const int a1i = indexOf(t, "a1");
    const Point a = centreOf(t[indexOf(t, "a")], t[a1i], 360, {0, 0});
    TEST_ASSERT_EQUAL(180, a.x);
    TEST_ASSERT_EQUAL(180 - ROW_STEP, a.y);
}

static void the_tile_under_a_point() {
    const std::vector<Tile> t = comb();
    const int b = indexOf(t, "b");
    TEST_ASSERT_EQUAL(b, tileAt(t, b, 360, {0, 0}, {180, 180}));
    TEST_ASSERT_EQUAL(indexOf(t, "c"), tileAt(t, b, 360, {0, 0}, {180 + COL_STEP, 190}));
    // Between two tiles: none.
    TEST_ASSERT_EQUAL(-1, tileAt(t, b, 360, {0, 0}, {180 + COL_STEP / 2, 180}));
    // Panned: what lay beyond is under the finger now.
    TEST_ASSERT_EQUAL(indexOf(t, "c0"), tileAt(t, b, 360, {-COL_STEP, ROW_STEP}, {180, 180}));
}

static void near_the_edge_the_map_pans_towards_the_finger_and_no_further_than_the_comb() {
    const std::vector<Tile> t = comb();
    const int b = indexOf(t, "b");
    // In the middle: still.
    Point p = panned(t, b, 360, {0, 0}, {200, 180}, 100);
    TEST_ASSERT_EQUAL(0, p.x);
    TEST_ASSERT_EQUAL(0, p.y);
    // At the right edge: the map moves left, a tenth of a second at 240/s.
    p = panned(t, b, 360, {0, 0}, {355, 180}, 100);
    TEST_ASSERT_EQUAL(-24, p.x);
    TEST_ASSERT_EQUAL(0, p.y);
    // Held there long: no further than C at the centre.
    p = panned(t, b, 360, {0, 0}, {355, 180}, 10000);
    TEST_ASSERT_EQUAL(-COL_STEP, p.x);
    // Down below the row on B: nothing lies below B's column, but A1 lies
    // below the row - the comb's lowest level bounds it.
    p = panned(t, b, 360, {0, 0}, {180, 355}, 10000);
    TEST_ASSERT_EQUAL(-ROW_STEP, p.y);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(the_row_across_and_what_lies_above_and_below_its_pages);
    RUN_TEST(the_current_screen_lies_in_the_middle);
    RUN_TEST(the_tile_under_a_point);
    RUN_TEST(near_the_edge_the_map_pans_towards_the_finger_and_no_further_than_the_comb);
    return UNITY_END();
}
