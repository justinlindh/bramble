#ifndef BRAMBLE_UI_H
#define BRAMBLE_UI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef enum {
    SCREEN_MAIN = 0,
    SCREEN_MESSAGES,
    SCREEN_NODES,
    SCREEN_COMPOSE,   /* T-Deck: compose; Heltec: Stats */
    SCREEN_SETTINGS,
    SCREEN_COUNT
} ui_screen_t;

/* Connectivity modes for WiFi/BLE switcher */
typedef enum {
    CONN_MODE_WIFI = 0,
    CONN_MODE_BLE = 1,
    CONN_MODE_BOTH = 2, /* legacy persisted value; normalized to WiFi */
    CONN_MODE_COUNT = 2 /* exposed modes are exclusive: WiFi or BLE */
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

/* Settings rows for non-graphical UI */
typedef enum {
    UI_SETTINGS_ITEM_CONN_MODE = 0,
    UI_SETTINGS_ITEM_OLED_ROTATION,
    UI_SETTINGS_ITEM_LOCATION,
    UI_SETTINGS_ITEM_COUNT
} ui_settings_item_t;

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

/* Compose buffer for keyboard input */
#define COMPOSE_BUF_SIZE 200

typedef struct {
    ui_screen_t current_screen;
    ui_screen_t prev_screen;
    uint32_t screen_enter_time;
    uint32_t last_activity;
    bool screen_dirty;
    ui_settings_item_t settings_item_cursor; /* selected settings row */
    int settings_cursor;        /* selected value while editing current row */
    bool settings_editing;      /* true when in settings edit mode */
    bool settings_confirmed;    /* set true on long-press confirm */
    bool pending_message_notification; /* set when a message arrives during active navigation */
    uint32_t message_auto_switch_time; /* timestamp when idle auto-switch to messages happened */

    /* Compose state */
    char compose_buf[COMPOSE_BUF_SIZE];
    int compose_len;
    bool compose_active;        /* true when in compose mode with cursor */
} ui_state_t;

void ui_init(ui_state_t *state);
void ui_handle_button(ui_state_t *state, ui_button_t btn, uint32_t now_ms);
ui_screen_t ui_get_screen(const ui_state_t *state);
bool ui_needs_redraw(const ui_state_t *state);
void ui_mark_drawn(ui_state_t *state);

/* Connectivity mode — NVS-persisted, applied on next boot.
 * Implemented in main/main.c; declared here so any UI component can call them. */
conn_mode_t conn_mode_get(void);
void conn_mode_set(conn_mode_t mode);

#define UI_INACTIVITY_TIMEOUT_MS 60000
#define UI_MESSAGE_IDLE_THRESHOLD_MS 10000
#define UI_MESSAGE_AUTO_RESTORE_TIMEOUT_MS 30000
void ui_check_timeout(ui_state_t *state, uint32_t now_ms);
void ui_on_message_received(ui_state_t *state, uint32_t now_ms);

// Display formatters
int ui_format_main_line1(const ui_main_data_t *data, char *buf, size_t buf_len);
int ui_format_main_line2(const ui_main_data_t *data, char *buf, size_t buf_len);
int ui_format_main_line3(const ui_main_data_t *data, char *buf, size_t buf_len);
int ui_format_uptime(uint32_t uptime_sec, char *buf, size_t buf_len);

/* Normalize persisted connectivity mode at boot.
 * Legacy WiFi+BLE values are coerced to WiFi (exclusive mode policy).
 */
conn_mode_t conn_mode_resolve_boot(conn_mode_t requested, bool low_sram_board);

#endif
