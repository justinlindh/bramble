#include "msg_store.h"
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_timer.h"
#endif

static stored_msg_t s_msgs[MSG_STORE_MAX];
static int s_head = 0;   /* Next write position */
static int s_count = 0;  /* Number of stored messages */

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

void msg_store_add_ex(uint32_t peer_addr, msg_direction_t dir,
                      const char *text, size_t text_len,
                      int8_t rssi, int8_t snr,
                      uint32_t packet_id, msg_status_t status) {
    stored_msg_t *m = &s_msgs[s_head];
    m->peer_addr = peer_addr;
    m->direction = dir;
    m->status = status;
    m->packet_id = packet_id;
    m->timestamp_s = get_uptime_s();
    m->rssi = rssi;
    m->snr = snr;

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
}

void msg_store_add(uint32_t peer_addr, msg_direction_t dir,
                   const char *text, size_t text_len,
                   int8_t rssi, int8_t snr) {
    msg_store_add_ex(peer_addr, dir, text, text_len, rssi, snr, 0, MSG_STATUS_NONE);
}

bool msg_store_update_status(uint32_t packet_id, msg_status_t status) {
    if (packet_id == 0) return false;
    int start = (s_head - s_count + MSG_STORE_MAX) % MSG_STORE_MAX;
    /* Search newest first for faster match */
    for (int i = s_count - 1; i >= 0; i--) {
        int idx = (start + i) % MSG_STORE_MAX;
        if (s_msgs[idx].packet_id == packet_id) {
            s_msgs[idx].status = status;
            return true;
        }
    }
    return false;
}

int msg_store_count(void) {
    return s_count;
}

const stored_msg_t *msg_store_get(int index) {
    if (index < 0 || index >= s_count) return NULL;

    int start = (s_head - s_count + MSG_STORE_MAX) % MSG_STORE_MAX;
    int actual = (start + index) % MSG_STORE_MAX;
    return &s_msgs[actual];
}

void msg_store_clear(void) {
    s_head = 0;
    s_count = 0;
}
