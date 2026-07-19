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

/**
 * SX1262 SetCadParams cadSymbolNum register value used by the driver.
 * The register encodes a power of two: 0 = 1 symbol, 1 = 2, 2 = 4, 3 = 8,
 * 4 = 16. Value 2 therefore means a 4-symbol CAD, not a 2-symbol one.
 */
#define BRAMBLE_CAD_SYMBOL_NUM_REG 2u

/** Fixed slack added to the CAD wait for IRQ delivery, SPI arbitration and
 *  scheduler latency, on top of the proportional margin. */
#define BRAMBLE_CAD_OVERHEAD_MS 10u

/** Lower bound on the derived CAD wait, so fast (low SF, wide BW) configs
 *  keep a usable amount of slack instead of a few hundred microseconds. */
#define BRAMBLE_CAD_TIMEOUT_MIN_MS 50u

/**
 * LoRa symbol time in microseconds: (2^sf) / bw_hz, rounded up.
 * sf is clamped to 5..12 and a zero bw_hz falls back to 125 kHz.
 */
uint32_t bramble_symbol_time_us(uint8_t sf, uint32_t bw_hz);

/**
 * Wall-clock budget for one radio_cad_check(), in milliseconds.
 *
 * The SX1262 samples cad_symbol_num_reg-encoded symbols before raising
 * CadDone, so the wait has to cover that span at the configured SF and
 * bandwidth. The budget is twice the raw CAD duration plus a fixed
 * overhead, floored at BRAMBLE_CAD_TIMEOUT_MIN_MS.
 */
uint32_t bramble_cad_timeout_ms(uint8_t sf, uint32_t bw_hz, uint8_t cad_symbol_num_reg);

#endif
