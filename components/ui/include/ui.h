#ifndef BRAMBLE_UI_H
#define BRAMBLE_UI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef enum {
    SCREEN_MAIN = 0,
    SCREEN_MESSAGES,
    SCREEN_NODES,
    SCREEN_COMPOSE, /* T-Deck: compose; Heltec: Stats */
    SCREEN_GPS,     /* GPS status - only reachable when gps_available is set */
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
    UI_SETTINGS_ITEM_GPS, /* only reachable when gps_available (board has GPS) */
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
    int settings_cursor;                     /* selected value while editing current row */
    bool settings_editing;                   /* true when in settings edit mode */
    bool settings_confirmed;                 /* set true on long-press confirm */
    int unread_count; /* messages arrived but not yet seen; rendered as a header badge */
    int msg_scroll;   /* messages scrolled back from newest on SCREEN_MESSAGES */
    int msg_total;    /* snapshot of msg_store_count(), set by the main loop */
    uint32_t message_auto_switch_time; /* timestamp when idle auto-switch to messages happened */
    bool gps_available; /* true when the board has GPS - gates SCREEN_GPS in the cycle */

    /* Compose state */
    char compose_buf[COMPOSE_BUF_SIZE];
    int compose_len;
    bool compose_active; /* true when in compose mode with cursor */

    /* Nodes screen: per-peer selection + SAS-verify sub-modes (text UI). */
    bool nodes_selecting;       /* cursor-selection mode active on SCREEN_NODES */
    int nodes_cursor;           /* selected neighbor index into the non-zero list */
    int node_total;             /* snapshot of selectable neighbor count (main loop feeds it) */
    bool node_detail_open;      /* viewing the selected peer's SAS detail */
    bool node_verify_armed;     /* first confirm press seen; next confirm commits */
    bool node_verify_confirmed; /* set on commit; main.c applies mesh_set_peer_verified then clears
                                 */
} ui_state_t;

void ui_init(ui_state_t* state);
void ui_handle_button(ui_state_t* state, ui_button_t btn, uint32_t now_ms);
ui_screen_t ui_get_screen(const ui_state_t* state);
bool ui_needs_redraw(const ui_state_t* state);
void ui_mark_drawn(ui_state_t* state);

/* Mark SCREEN_GPS as reachable in the screen cycle. Defaults to false (unset by
 * ui_init), so callers on non-GPS boards never need to touch this. */
void ui_set_gps_available(ui_state_t* state, bool available);

/* Snapshot of msg_store_count(), fed by the main loop so the button
 * handler can clamp scrollback without a msg_store dependency. */
void ui_set_message_total(ui_state_t* state, int total);

/* Snapshot of the selectable neighbor count, fed by the main loop so the button
 * handler can clamp/wrap the nodes cursor without a mesh dependency. */
void ui_set_node_total(ui_state_t* state, int total);

/* Connectivity mode, NVS-persisted, applied on next boot.
 * Implemented in main/main.c; declared here so any UI component can call them. */
conn_mode_t conn_mode_get(void);
void conn_mode_set(conn_mode_t mode);

#define UI_INACTIVITY_TIMEOUT_MS 60000
/* Reading is idle time: give the messages screen a much longer leash. */
#define UI_MESSAGES_INACTIVITY_TIMEOUT_MS 300000
#define UI_MSG_PAGE_LINES 4 /* message lines visible on the 128x64 OLED */
#define UI_MESSAGE_IDLE_THRESHOLD_MS 10000
#define UI_MESSAGE_AUTO_RESTORE_TIMEOUT_MS 30000
void ui_check_timeout(ui_state_t* state, uint32_t now_ms);
void ui_on_message_received(ui_state_t* state, uint32_t now_ms);

/* One rendered message line for the text (OLED) UI. All lookups
 * (beacon name, channel name, status badge) happen in the caller;
 * this stays a pure, host-testable formatter. */
typedef struct {
    const char* text;
    int text_len;
    bool outgoing;
    uint32_t peer_addr;
    const char* peer_name;    /* NULL or "" when unknown */
    int channel_index;        /* <= 0: default/none, no tag */
    const char* channel_name; /* NULL or "": fall back to "#<index>" */
    const char* badge;        /* "", " *", " +", "++", " x" */
    int age_s;                /* seconds since stored; < 0 hides the age suffix */
} ui_msg_line_t;

int ui_format_msg_line(const ui_msg_line_t* m, char* buf, size_t buf_len);

// Display formatters
int ui_format_main_line1(const ui_main_data_t* data, char* buf, size_t buf_len);
int ui_format_main_line2(const ui_main_data_t* data, char* buf, size_t buf_len);
int ui_format_main_line3(const ui_main_data_t* data, char* buf, size_t buf_len);
int ui_format_uptime(uint32_t uptime_sec, char* buf, size_t buf_len);

/* Normalize persisted connectivity mode at boot.
 * Legacy WiFi+BLE values are coerced to WiFi (exclusive mode policy).
 */
conn_mode_t conn_mode_resolve_boot(conn_mode_t requested, bool low_sram_board);

#endif
