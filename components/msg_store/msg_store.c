#include "msg_store.h"
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_timer.h"
#endif

#ifdef CONFIG_BRAMBLE_MSG_PERSIST_ENABLED
#include "msg_store_spiffs.h"
#endif

static stored_msg_t s_msgs[MSG_STORE_MAX];
static int s_head = 0;  /* Next write position */
static int s_count = 0; /* Number of stored messages */

static uint32_t get_uptime_s(void) {
#ifdef ESP_PLATFORM
    return (uint32_t)(esp_timer_get_time() / 1000000ULL);
#else
    return 0;
#endif
}

void msg_store_init(void) {
    memset(s_msgs, 0, sizeof(s_msgs));
    s_head = 0;
    s_count = 0;
}

void msg_store_add_ex2(uint32_t peer_addr, msg_direction_t dir, const char* text, size_t text_len,
                       int8_t rssi, int8_t snr, uint32_t packet_id, msg_status_t status,
                       int16_t channel_index) {
    stored_msg_t* m = &s_msgs[s_head];
    memset(m, 0, sizeof(*m));
    m->peer_addr = peer_addr;
    m->direction = dir;
    m->status = status;
    m->packet_id = packet_id;
    m->timestamp_s = get_uptime_s();
    m->rssi = rssi;
    m->snr = snr;
    m->channel_index = channel_index;

    if (text_len >= MSG_TEXT_MAX) {
        text_len = MSG_TEXT_MAX - 1;
    }
    memcpy(m->text, text, text_len);
    m->text[text_len] = '\0';
    m->text_len = (uint16_t)text_len;

    s_head = (s_head + 1) % MSG_STORE_MAX;
    if (s_count < MSG_STORE_MAX) {
        s_count++;
    }

#ifdef CONFIG_BRAMBLE_MSG_PERSIST_ENABLED
    /* Persist to SPIFFS */
    if (msg_store_spiffs_save(m) == 0) {
        /* Check if rollover needed */
        int total = msg_store_spiffs_get_count();
        if (total >= CONFIG_BRAMBLE_MSG_PERSIST_MAX) {
            msg_store_spiffs_rollover(CONFIG_BRAMBLE_MSG_PERSIST_MAX,
                                      CONFIG_BRAMBLE_MSG_PERSIST_ROLLOVER_KEEP_PCT);
        }
    }
#endif
}

void msg_store_add_ex(uint32_t peer_addr, msg_direction_t dir, const char* text, size_t text_len,
                      int8_t rssi, int8_t snr, uint32_t packet_id, msg_status_t status) {
    msg_store_add_ex2(peer_addr, dir, text, text_len, rssi, snr, packet_id, status, -1);
}

void msg_store_add(uint32_t peer_addr, msg_direction_t dir, const char* text, size_t text_len,
                   int8_t rssi, int8_t snr) {
    msg_store_add_ex(peer_addr, dir, text, text_len, rssi, snr, 0, MSG_STATUS_NONE);
}

bool msg_store_update_status_with_route(uint32_t packet_id, msg_status_t status,
                                        uint8_t route_hop_count, const uint32_t* route_hops) {
    if (packet_id == 0)
        return false;
    int start = (s_head - s_count + MSG_STORE_MAX) % MSG_STORE_MAX;
    /* Search newest first for faster match */
    for (int i = s_count - 1; i >= 0; i--) {
        int idx = (start + i) % MSG_STORE_MAX;
        if (s_msgs[idx].packet_id == packet_id) {
            s_msgs[idx].status = status;
            if (route_hops && route_hop_count > 0) {
                uint8_t bounded =
                    (route_hop_count > MSG_ROUTE_MAX_HOPS) ? MSG_ROUTE_MAX_HOPS : route_hop_count;
                s_msgs[idx].route_hop_count = bounded;
                for (uint8_t h = 0; h < bounded; h++) {
                    s_msgs[idx].route_hops[h] = route_hops[h];
                }
            }
            return true;
        }
    }
    return false;
}

bool msg_store_update_status(uint32_t packet_id, msg_status_t status) {
    return msg_store_update_status_with_route(packet_id, status, 0, NULL);
}

int msg_store_count(void) { return s_count; }

const stored_msg_t* msg_store_get(int index) {
    if (index < 0 || index >= s_count)
        return NULL;

    int start = (s_head - s_count + MSG_STORE_MAX) % MSG_STORE_MAX;
    int actual = (start + index) % MSG_STORE_MAX;
    return &s_msgs[actual];
}

void msg_store_clear(void) {
    s_head = 0;
    s_count = 0;
}

void msg_store_init_with_persistence(void) {
    msg_store_init();

#ifdef CONFIG_BRAMBLE_MSG_PERSIST_ENABLED
    /* Initialize SPIFFS persistence */
    if (msg_store_spiffs_init() == 0) {
        /*
         * ROOT-CAUSE FIX:
         * Do NOT allocate stored_msg_t loaded[MSG_STORE_MAX] on stack here.
         * app_main runs with a small default main-task stack (often 3584 bytes).
         * A local loaded[] buffer is ~4.4KB (20 * sizeof(stored_msg_t)) and can
         * overflow main-task stack, causing later boot-stage crashes.
         */
        int count = msg_store_spiffs_load_recent(s_msgs, MSG_STORE_MAX);
        if (count < 0)
            count = 0;
        if (count > MSG_STORE_MAX)
            count = MSG_STORE_MAX;

        s_count = count;
        s_head = count % MSG_STORE_MAX;
    }
#endif
}
