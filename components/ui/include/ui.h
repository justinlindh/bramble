#ifndef BRAMBLE_UI_H
#define BRAMBLE_UI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef enum {
    SCREEN_MAIN = 0,
    SCREEN_MESSAGES,
    SCREEN_COMPOSE,
    SCREEN_NODES,
    SCREEN_SETTINGS,
    SCREEN_COUNT
} ui_screen_t;

/* Connectivity modes for WiFi/BLE switcher */
typedef enum {
    CONN_MODE_WIFI = 0,
    CONN_MODE_BLE,
    CONN_MODE_BOTH,
    CONN_MODE_COUNT
} conn_mode_t;

typedef enum {
    BTN_NONE = 0,
    BTN_SHORT_PRESS,
    BTN_LONG_PRESS,
    BTN_DOUBLE_PRESS,
    /* Trackball directions (T-Deck Plus) */
    BTN_UP,
    BTN_DOWN,
    BTN_LEFT,
    BTN_RIGHT,
    BTN_SELECT,
} ui_button_t;

typedef struct {
    uint32_t my_addr;
    uint8_t battery_pct;
    uint8_t neighbor_count;
    uint32_t uptime_sec;
    uint8_t tx_queue_depth;
} ui_main_data_t;

typedef struct {
    int total_messages;
    int scroll_pos;
} ui_messages_data_t;

typedef struct {
    int total_nodes;
    int scroll_pos;
} ui_nodes_data_t;

typedef struct {
    ui_screen_t current_screen;
    ui_screen_t prev_screen;
    uint32_t screen_enter_time;
    uint32_t last_activity;
    bool screen_dirty;
    int settings_cursor;        /* selected option on Settings screen */
    bool settings_editing;      /* true when in settings edit mode */
    bool settings_confirmed;    /* set true on long-press confirm */
} ui_state_t;

void ui_init(ui_state_t *state);
void ui_handle_button(ui_state_t *state, ui_button_t btn, uint32_t now_ms);
ui_screen_t ui_get_screen(const ui_state_t *state);
bool ui_needs_redraw(const ui_state_t *state);
void ui_mark_drawn(ui_state_t *state);

#define UI_INACTIVITY_TIMEOUT_MS 60000
void ui_check_timeout(ui_state_t *state, uint32_t now_ms);

// Display formatters
int ui_format_main_line1(const ui_main_data_t *data, char *buf, size_t buf_len);
int ui_format_main_line2(const ui_main_data_t *data, char *buf, size_t buf_len);
int ui_format_main_line3(const ui_main_data_t *data, char *buf, size_t buf_len);
int ui_format_uptime(uint32_t uptime_sec, char *buf, size_t buf_len);

#endif
