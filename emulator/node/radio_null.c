/*
 * Spike-only no-op radio driver for the IDF linux target.
 *
 * Implements components/radio/include/radio.h (and radio_transmit_raw from
 * radio_internal.h) function-for-function: every call succeeds, TX completes
 * instantly, the RX callback never fires, CAD always reports a clear channel.
 * Replaced by radio_virt.c (emu_link ether client) in a later task.
 */
#include "radio.h"

#include <string.h>
#include <stdatomic.h>

#include "esp_log.h"

static const char* TAG = "radio_null";

static radio_config_t s_config;
static _Atomic radio_state_t s_state = RADIO_STATE_IDLE;
static radio_rx_callback_t s_rx_cb;
static radio_tx_done_callback_t s_tx_done_cb;
static radio_cad_done_callback_t s_cad_done_cb;

int radio_init(const radio_config_t* config) {
    s_config = *config;
    atomic_store(&s_state, RADIO_STATE_IDLE);
    ESP_LOGI(TAG, "null radio up: %.1f MHz SF%u BW %lu", (double)config->frequency_mhz,
             config->sf, (unsigned long)config->bw_hz);
    return 0;
}

int radio_reconfigure(const radio_config_t* config) {
    s_config = *config;
    return 0;
}

void radio_get_config(radio_config_t* config) { *config = s_config; }

/* Values must stay identical to radio_esp.c radio_get_profile_config(). */
void radio_get_profile_config(radio_profile_t profile, radio_config_t* config) {
    memset(config, 0, sizeof(*config));
    config->frequency_mhz = 915.0f;
    config->sync_word = 0x12;
    config->crc = true;
    config->explicit_header = true;

    switch (profile) {
    case RADIO_PROFILE_LONG_RANGE:
        config->sf = 10;
        config->bw_hz = 125000;
        config->coding_rate = 1; /* 4/5 */
        config->tx_power = 22;
        config->preamble = 12;
        break;
    case RADIO_PROFILE_MEDIUM_RANGE:
    default:
        config->sf = 7;
        config->bw_hz = 250000;
        config->coding_rate = 1;
        config->tx_power = 17;
        config->preamble = 8;
        break;
    }
}

void radio_start_rx(void) { atomic_store(&s_state, RADIO_STATE_RX); }

void radio_cad(void) {
    /* Channel is always clear on the null ether. */
    if (s_cad_done_cb) {
        s_cad_done_cb(false);
    }
}

bool radio_cad_check(void) { return false; }

void radio_set_tx_power(int8_t power) { s_config.tx_power = power; }

radio_state_t radio_get_state(void) { return atomic_load(&s_state); }

void radio_set_rx_callback(radio_rx_callback_t cb) { s_rx_cb = cb; }

void radio_set_tx_done_callback(radio_tx_done_callback_t cb) { s_tx_done_cb = cb; }

void radio_set_cad_done_callback(radio_cad_done_callback_t cb) { s_cad_done_cb = cb; }

void radio_sleep(void) { atomic_store(&s_state, RADIO_STATE_SLEEP); }

bool radio_check_and_clear_reinit(void) { return false; }

/* radio_internal.h: synchronous TX, mirrors radio_esp.c semantics (blocks
 * until TX done, fires the tx_done callback, returns to RX). */
int radio_transmit_raw(const uint8_t* data, uint8_t len) {
    (void)data;
    atomic_store(&s_state, RADIO_STATE_TX);
    ESP_LOGD(TAG, "TX %u bytes into the void", len);
    if (s_tx_done_cb) {
        s_tx_done_cb();
    }
    atomic_store(&s_state, RADIO_STATE_RX);
    return 0;
}
