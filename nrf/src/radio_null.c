// Null radio backend for P0: satisfies radio.h and radio_transmit_raw at link
// time so the portable stack (tx_gate included) compiles and boots before the
// LR1110 backend exists. Transmit fails, CAD reports clear air, RX never
// fires. Replaced wholesale by the SWDR001/LR1110 backend in P1; this is the
// P0-complete radio by phase definition, not a stub of it.
// radio_get_profile_config lives in radio_profiles.c (portable) and is NOT
// defined here.
#include <stddef.h>

#include "esp_log.h"
#include "radio.h"
#include "radio_internal.h"

static const char* TAG = "radio";

static radio_config_t s_cfg;

int radio_init(const radio_config_t* config) {
    if (config != NULL) {
        s_cfg = *config;
    }
    ESP_LOGI(TAG, "null backend (P0, no radio hardware driver yet)");
    return 0;
}

int radio_reconfigure(const radio_config_t* config) {
    if (config != NULL) {
        s_cfg = *config;
    }
    return 0;
}

void radio_get_config(radio_config_t* config) {
    if (config != NULL) {
        *config = s_cfg;
    }
}

void radio_start_rx(void) {}

void radio_cad(void) {}

bool radio_cad_check(void) {
    // "Channel clear": lets tx_gate proceed to radio_transmit_raw, whose -1
    // return is the honest "no radio" signal callers see and log.
    return false;
}

void radio_set_tx_power(int8_t power) { (void)power; }

radio_state_t radio_get_state(void) { return RADIO_STATE_IDLE; }

void radio_set_rx_callback(radio_rx_callback_t cb) { (void)cb; }

void radio_set_tx_done_callback(radio_tx_done_callback_t cb) { (void)cb; }

void radio_sleep(void) {}

bool radio_check_and_clear_reinit(void) { return false; }

int radio_transmit_raw(const uint8_t* data, uint8_t len) {
    (void)data;
    (void)len;
    return -1;
}
