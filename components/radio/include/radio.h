#ifndef BRAMBLE_RADIO_H
#define BRAMBLE_RADIO_H

#include <stdint.h>
#include <stdbool.h>

typedef enum { RADIO_PROFILE_LONG_RANGE, RADIO_PROFILE_MEDIUM_RANGE } radio_profile_t;

typedef enum {
    RADIO_STATE_IDLE,
    RADIO_STATE_TX,
    RADIO_STATE_RX,
    RADIO_STATE_CAD,
    RADIO_STATE_SLEEP
} radio_state_t;

typedef struct {
    int16_t rssi;
    int8_t snr;
    uint8_t len;
} radio_rx_info_t;

typedef struct {
    float frequency_mhz;
    uint8_t sf;
    uint32_t bw_hz;
    uint8_t coding_rate;
    int8_t tx_power;
    uint16_t preamble;
    uint8_t sync_word;
    bool crc;
    bool explicit_header;
} radio_config_t;

typedef void (*radio_rx_callback_t)(const uint8_t* data, uint8_t len, const radio_rx_info_t* info);
typedef void (*radio_tx_done_callback_t)(void);
typedef void (*radio_cad_done_callback_t)(bool detected);

int radio_init(const radio_config_t* config);
int radio_reconfigure(const radio_config_t* config);
void radio_get_config(radio_config_t* config);
void radio_get_profile_config(radio_profile_t profile, radio_config_t* config);
int radio_transmit(const uint8_t* data, uint8_t len);
void radio_start_rx(void);
void radio_cad(void);
bool radio_cad_check(void);
void radio_set_tx_power(int8_t power);
radio_state_t radio_get_state(void);
void radio_set_rx_callback(radio_rx_callback_t cb);
void radio_set_tx_done_callback(radio_tx_done_callback_t cb);
void radio_set_cad_done_callback(radio_cad_done_callback_t cb);
void radio_sleep(void);

/**
 * Check and clear the radio-needs-reinit flag.
 * Returns true if the SX1262 was hard-reset due to stuck BUSY and
 * needs full reconfiguration (radio_reconfigure).
 */
bool radio_check_and_clear_reinit(void);

uint32_t bramble_calculate_airtime_us(uint16_t payload_bytes, uint8_t sf, uint32_t bw_hz,
                                      uint8_t cr);

#endif
