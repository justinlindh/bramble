#include "unity.h"

#include "board_config.h"
#include "driver/gpio.h"

#include <string.h>

/* ---- controllable stubs ---- */
static bramble_board_config_t g_cfg;
static gpio_config_t g_last_gpio_cfg;
static int g_gpio_config_calls;
static int g_gpio_level = 1; /* active-low, 1 = released */

const bramble_board_config_t* board_get_config(void) { return &g_cfg; }

esp_err_t gpio_config(const gpio_config_t* pGPIOConfig) {
    g_gpio_config_calls++;
    if (pGPIOConfig) {
        g_last_gpio_cfg = *pGPIOConfig;
    }
    return ESP_OK;
}

esp_err_t gpio_set_level(gpio_num_t gpio_num, uint32_t level) {
    (void)gpio_num;
    g_gpio_level = (int)level;
    return ESP_OK;
}

int gpio_get_level(gpio_num_t gpio_num) {
    (void)gpio_num;
    return g_gpio_level;
}

#include "../../components/button/button.c"

static void reset_button_state(void) {
    memset(&g_cfg, 0, sizeof(g_cfg));
    g_cfg.button_gpio = 4;
    memset(&g_last_gpio_cfg, 0, sizeof(g_last_gpio_cfg));
    g_gpio_config_calls = 0;
    g_gpio_level = 1;

    last_pressed = false;
    press_start_ms = 0;
    last_release_ms = 0;
    waiting_double = false;
    s_board = NULL;
}

void setUp(void) {
    reset_button_state();
    button_init();
}

void tearDown(void) {}

void test_button_init_configures_input_pullup(void) {
    TEST_ASSERT_EQUAL_INT(1, g_gpio_config_calls);
    TEST_ASSERT_EQUAL_UINT64((1ULL << g_cfg.button_gpio), g_last_gpio_cfg.pin_bit_mask);
    TEST_ASSERT_EQUAL_INT(GPIO_MODE_INPUT, g_last_gpio_cfg.mode);
    TEST_ASSERT_EQUAL_INT(GPIO_PULLUP_ENABLE, g_last_gpio_cfg.pull_up_en);
}

void test_debounce_filters_rapid_toggle(void) {
    g_gpio_level = 0;
    TEST_ASSERT_EQUAL_INT(BTN_NONE, button_poll(100));

    g_gpio_level = 1;
    TEST_ASSERT_EQUAL_INT(BTN_NONE, button_poll(120)); /* 20ms hold < debounce */
    TEST_ASSERT_FALSE(waiting_double);

    TEST_ASSERT_EQUAL_INT(BTN_NONE, button_poll(500));
}

void test_short_press_detected_after_double_gap_timeout(void) {
    g_gpio_level = 0;
    TEST_ASSERT_EQUAL_INT(BTN_NONE, button_poll(0));

    g_gpio_level = 1;
    TEST_ASSERT_EQUAL_INT(BTN_NONE, button_poll(120));
    TEST_ASSERT_TRUE(waiting_double);

    TEST_ASSERT_EQUAL_INT(BTN_SHORT_PRESS, button_poll(421));
    TEST_ASSERT_FALSE(waiting_double);
}

void test_long_press_detected_on_release_at_threshold(void) {
    g_gpio_level = 0;
    TEST_ASSERT_EQUAL_INT(BTN_NONE, button_poll(1000));

    g_gpio_level = 1;
    TEST_ASSERT_EQUAL_INT(BTN_LONG_PRESS, button_poll(1800));
    TEST_ASSERT_FALSE(waiting_double);
}

void test_double_press_state_transition(void) {
    g_gpio_level = 0;
    TEST_ASSERT_EQUAL_INT(BTN_NONE, button_poll(0));
    g_gpio_level = 1;
    TEST_ASSERT_EQUAL_INT(BTN_NONE, button_poll(100));
    TEST_ASSERT_TRUE(waiting_double);

    g_gpio_level = 0;
    TEST_ASSERT_EQUAL_INT(BTN_NONE, button_poll(200));
    g_gpio_level = 1;
    TEST_ASSERT_EQUAL_INT(BTN_DOUBLE_PRESS, button_poll(260));
    TEST_ASSERT_FALSE(waiting_double);
}

void test_press_without_release_emits_no_event(void) {
    g_gpio_level = 0;
    TEST_ASSERT_EQUAL_INT(BTN_NONE, button_poll(0));
    TEST_ASSERT_EQUAL_INT(BTN_NONE, button_poll(500));
    TEST_ASSERT_EQUAL_INT(BTN_NONE, button_poll(1500));
    TEST_ASSERT_TRUE(last_pressed);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_button_init_configures_input_pullup);
    RUN_TEST(test_debounce_filters_rapid_toggle);
    RUN_TEST(test_short_press_detected_after_double_gap_timeout);
    RUN_TEST(test_long_press_detected_on_release_at_threshold);
    RUN_TEST(test_double_press_state_transition);
    RUN_TEST(test_press_without_release_emits_no_event);
    return UNITY_END();
}
