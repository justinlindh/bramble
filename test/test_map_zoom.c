/* Map zoom ladder: the arithmetic, the readout and the scale bar behind the
 * Map screen's zoom controls. scr_map.c is LVGL and not host-linkable, so the
 * pure part lives in map_zoom.c and is tested here (same split as
 * test_chat_message_ui). */
#include "unity.h"
#include "map_zoom.h"
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* Unity ships with double comparisons excluded, so levels are asserted in
 * whole metres, which every rung is. */
static int level_m(int idx) { return (int)(map_zoom_level_km(idx) * 1000.0 + 0.5); }

static const int LADDER_M[MAP_ZOOM_LEVEL_COUNT] = {50,   100,  250,   500,   1000,
                                                   2000, 5000, 10000, 25000, 50000};

void test_levels_ascend_and_clamp(void) {
    for (int i = 0; i < MAP_ZOOM_LEVEL_COUNT; i++) {
        TEST_ASSERT_EQUAL_INT(LADDER_M[i], level_m(i));
    }
    /* Out of range reads the nearest end, never off the array. */
    TEST_ASSERT_EQUAL_INT(LADDER_M[0], level_m(-1));
    TEST_ASSERT_EQUAL_INT(LADDER_M[0], level_m(-99));
    TEST_ASSERT_EQUAL_INT(LADDER_M[MAP_ZOOM_LEVEL_COUNT - 1], level_m(MAP_ZOOM_LEVEL_COUNT));
}

void test_index_for_km_picks_the_first_level_that_fits(void) {
    TEST_ASSERT_EQUAL_INT(1, map_zoom_index_for_km(0.1)); /* exact match, not the next rung */
    TEST_ASSERT_EQUAL_INT(3, map_zoom_index_for_km(0.5));
    TEST_ASSERT_EQUAL_INT(4, map_zoom_index_for_km(0.6));
    TEST_ASSERT_EQUAL_INT(6, map_zoom_index_for_km(4.9));
    TEST_ASSERT_EQUAL_INT(MAP_ZOOM_LEVEL_COUNT - 1, map_zoom_index_for_km(50.0));
    /* Nothing on the ladder is wide enough: pin to the widest. */
    TEST_ASSERT_EQUAL_INT(MAP_ZOOM_LEVEL_COUNT - 1, map_zoom_index_for_km(500.0));
}

/* Auto-fit resolves its needed half-width through this call, so a mesh whose
 * peers are metres apart has to land on the tight rungs rather than being
 * floored at a wide one. */
void test_index_for_km_reaches_the_tight_rungs(void) {
    TEST_ASSERT_EQUAL_INT(0, map_zoom_index_for_km(0.05));  /* exact tightest */
    TEST_ASSERT_EQUAL_INT(0, map_zoom_index_for_km(0.012)); /* peers ~10 m apart, 20% margin */
    TEST_ASSERT_EQUAL_INT(0, map_zoom_index_for_km(0.0));   /* tighter than the floor pins to it */
    TEST_ASSERT_EQUAL_INT(1, map_zoom_index_for_km(0.06));
    TEST_ASSERT_EQUAL_INT(2, map_zoom_index_for_km(0.2));
    TEST_ASSERT_EQUAL_INT(50, level_m(map_zoom_index_for_km(0.03)));
}

void test_step_walks_one_rung_each_way(void) {
    TEST_ASSERT_EQUAL_INT(2, map_zoom_step(3, -1, 5.0));
    TEST_ASSERT_EQUAL_INT(4, map_zoom_step(3, +1, 5.0));
}

void test_step_clamps_at_both_ends(void) {
    TEST_ASSERT_EQUAL_INT(0, map_zoom_step(0, -1, 5.0));
    TEST_ASSERT_EQUAL_INT(MAP_ZOOM_LEVEL_COUNT - 1,
                          map_zoom_step(MAP_ZOOM_LEVEL_COUNT - 1, +1, 5.0));
    TEST_ASSERT_FALSE(map_zoom_can_step(0, -1, 5.0));
    TEST_ASSERT_FALSE(map_zoom_can_step(MAP_ZOOM_LEVEL_COUNT - 1, +1, 5.0));
    TEST_ASSERT_TRUE(map_zoom_can_step(0, +1, 5.0));
    TEST_ASSERT_TRUE(map_zoom_can_step(MAP_ZOOM_LEVEL_COUNT - 1, -1, 5.0));

    /* Named ends, so a ladder edited at either end still has to hold its
     * floor and ceiling rather than quietly running off the array. */
    TEST_ASSERT_EQUAL_INT(50, level_m(map_zoom_step(0, -1, 5.0)));
    TEST_ASSERT_EQUAL_INT(50000, level_m(map_zoom_step(MAP_ZOOM_LEVEL_COUNT - 1, +1, 5.0)));
}

void test_a_walk_from_end_to_end_visits_every_rung(void) {
    int idx = MAP_ZOOM_LEVEL_COUNT - 1;
    for (int i = MAP_ZOOM_LEVEL_COUNT - 1; i >= 0; i--) {
        TEST_ASSERT_EQUAL_INT(LADDER_M[i], level_m(idx));
        idx = map_zoom_step(idx, -1, 5.0);
    }
    TEST_ASSERT_EQUAL_INT(0, idx); /* parked on the floor, not past it */
}

void test_leaving_auto_steps_one_rung_from_where_auto_sits(void) {
    /* Auto resolving to 5 km is index 6, so the first press must land on 2 km
     * (in) or 10 km (out), not at the end of the ladder. */
    TEST_ASSERT_EQUAL_INT(5, map_zoom_step(MAP_ZOOM_AUTO, -1, 5.0));
    TEST_ASSERT_EQUAL_INT(7, map_zoom_step(MAP_ZOOM_AUTO, +1, 5.0));
    TEST_ASSERT_EQUAL_INT(2000, level_m(map_zoom_step(MAP_ZOOM_AUTO, -1, 5.0)));

    /* Auto sitting on a rung end still steps to a real index: leaving auto
     * pins the window, which is a state change even when the level holds. */
    TEST_ASSERT_EQUAL_INT(0, map_zoom_step(MAP_ZOOM_AUTO, -1, 0.05));
    TEST_ASSERT_TRUE(map_zoom_can_step(MAP_ZOOM_AUTO, -1, 0.05));
    TEST_ASSERT_EQUAL_INT(MAP_ZOOM_LEVEL_COUNT - 1, map_zoom_step(MAP_ZOOM_AUTO, +1, 50.0));
    TEST_ASSERT_TRUE(map_zoom_can_step(MAP_ZOOM_AUTO, +1, 50.0));
}

void test_readout_names_the_mode_and_the_level(void) {
    char buf[24];

    map_zoom_format(buf, sizeof(buf), MAP_ZOOM_AUTO, 5.0);
    TEST_ASSERT_EQUAL_STRING("auto 5 km", buf);

    map_zoom_format(buf, sizeof(buf), 5, 2.0);
    TEST_ASSERT_EQUAL_STRING("manual 2 km", buf);

    /* Sub-kilometre reads in metres, like the scale bar. */
    map_zoom_format(buf, sizeof(buf), 3, 0.5);
    TEST_ASSERT_EQUAL_STRING("manual 500 m", buf);
    map_zoom_format(buf, sizeof(buf), MAP_ZOOM_AUTO, 0.5);
    TEST_ASSERT_EQUAL_STRING("auto 500 m", buf);

    /* The tight rungs round to whole metres rather than truncating: 0.05 is
     * not exactly representable, so a truncating format would read 49 m. */
    map_zoom_format(buf, sizeof(buf), 0, 0.05);
    TEST_ASSERT_EQUAL_STRING("manual 50 m", buf);
    map_zoom_format(buf, sizeof(buf), 2, 0.25);
    TEST_ASSERT_EQUAL_STRING("manual 250 m", buf);

    /* Every rung has a readout that fits the label buffer the screen uses. */
    for (int i = 0; i < MAP_ZOOM_LEVEL_COUNT; i++) {
        map_zoom_format(buf, sizeof(buf), i, map_zoom_level_km(i));
        TEST_ASSERT_TRUE(strlen(buf) > 0 && strlen(buf) < sizeof(buf) - 1);
    }
}

/* The scale bar is the only thing on screen that says how far across the map
 * is, so a rung where it silently draws nothing is a rung with no scale at
 * all. That is exactly what happens if the ladder gains a level tighter than
 * the shortest distance the bar is willing to draw. */
void test_scale_bar_is_drawn_and_legible_at_every_rung(void) {
    for (int i = 0; i < MAP_ZOOM_LEVEL_COUNT; i++) {
        int px = 0;
        char label[16] = {0};
        TEST_ASSERT_TRUE_MESSAGE(
            map_zoom_scale_bar(map_zoom_level_km(i), &px, label, sizeof(label)),
            "a ladder rung with no scale bar");
        TEST_ASSERT_TRUE(px >= 20 && px <= 90);
        TEST_ASSERT_TRUE(strlen(label) > 0 && strlen(label) < sizeof(label) - 1);
        /* Never a bar labelled as no distance at all. */
        TEST_ASSERT_TRUE(strncmp(label, "0 ", 2) != 0);
    }
}

void test_scale_bar_labels_the_round_distance_it_drew(void) {
    int px = 0;
    char label[16] = {0};

    TEST_ASSERT_TRUE(map_zoom_scale_bar(0.05, &px, label, sizeof(label)));
    TEST_ASSERT_EQUAL_STRING("20 m", label);
    TEST_ASSERT_EQUAL_INT(56, px); /* 0.02 km at 140 px per 0.05 km */

    TEST_ASSERT_TRUE(map_zoom_scale_bar(0.1, &px, label, sizeof(label)));
    TEST_ASSERT_EQUAL_STRING("50 m", label);

    TEST_ASSERT_TRUE(map_zoom_scale_bar(0.5, &px, label, sizeof(label)));
    TEST_ASSERT_EQUAL_STRING("200 m", label);

    TEST_ASSERT_TRUE(map_zoom_scale_bar(50.0, &px, label, sizeof(label)));
    TEST_ASSERT_EQUAL_STRING("20 km", label);

    /* A nonsensical window draws no bar rather than dividing by zero. */
    TEST_ASSERT_FALSE(map_zoom_scale_bar(0.0, &px, label, sizeof(label)));
    TEST_ASSERT_FALSE(map_zoom_scale_bar(-1.0, &px, label, sizeof(label)));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_levels_ascend_and_clamp);
    RUN_TEST(test_index_for_km_picks_the_first_level_that_fits);
    RUN_TEST(test_index_for_km_reaches_the_tight_rungs);
    RUN_TEST(test_step_walks_one_rung_each_way);
    RUN_TEST(test_step_clamps_at_both_ends);
    RUN_TEST(test_a_walk_from_end_to_end_visits_every_rung);
    RUN_TEST(test_leaving_auto_steps_one_rung_from_where_auto_sits);
    RUN_TEST(test_readout_names_the_mode_and_the_level);
    RUN_TEST(test_scale_bar_is_drawn_and_legible_at_every_rung);
    RUN_TEST(test_scale_bar_labels_the_round_distance_it_drew);
    return UNITY_END();
}
