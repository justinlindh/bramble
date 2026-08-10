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

/** Longest chip-specific detail string a driver may report. Sized so the
 *  SX1262 driver's line, whose worst case is every device-error flag named in
 *  full plus the chip mode, command status and PA settings, cannot truncate. */
#define RADIO_HEALTH_DETAIL_MAX 192

/**
 * What the radio will tell us about its own transmit path.
 *
 * On no supported part can the commanded or the radiated output power be read
 * back: the SX1262's SetTxParams and SetPaConfig are write-only op-codes and
 * no register reports output power. So this reports the next best evidence,
 * as verdicts rather than one chip's register layout, because every LoRa part
 * this firmware drives has its own error word and its own PA configuration.
 * Confirming the level actually radiated needs external instrumentation.
 *
 * The verdicts are deliberately generic so the RPC layer can serialize them
 * without knowing which radio answered. Each driver maps its own registers
 * onto them and puts the raw values in `detail` for a human to read.
 */
typedef struct {
    bool supported;      /* false where there is no real chip to ask */
    const char* chip;    /* part name, NULL when unsupported */
    int8_t tx_power_dbm; /* level the driver programmed, not a measurement */

    /* The power amplifier did not ramp for a transmit, so nothing usable went
     * on air. The strongest evidence a chip can give that the commanded power
     * is not being produced. */
    bool pa_fault;
    /* These two also stop transmission rather than merely degrading it, and are
     * rarer than a PA fault rather than milder: with no synthesizer lock there
     * is no frequency to sit on, and with no reference there is no clock. */
    bool pll_fault;        /* synthesizer did not lock */
    bool oscillator_fault; /* reference oscillator did not start */

    /* The quiet one: costs link budget while every later command still
     * succeeds, so it surfaces nowhere else. */
    bool calibration_fault;

    /* Configuration written to the chip reads back as programmed. False means
     * config writes are not landing, which caps output well below the
     * commanded level. */
    bool config_verified;

    /* Chip-specific supporting values, human readable, never parsed. */
    char detail[RADIO_HEALTH_DETAIL_MAX];
} radio_health_t;

/**
 * Read the transmit-path evidence above off the chip.
 * Returns 0 on success. On drivers with no real radio behind them, fills in
 * supported=false and returns 0.
 */
int radio_get_health(radio_health_t* health);

/**
 * Output-power range the radio actually accepts, in dBm.
 *
 * This is a hardware limit and is distinct from the frequency plan's
 * regulatory ceiling, which is higher than any part's capability in US915 and
 * AU915. Callers that persist or report a requested power must respect both,
 * or they will store and echo a number the chip was never programmed with.
 */
int8_t radio_tx_power_min_dbm(void);
int8_t radio_tx_power_max_dbm(void);

/**
 * Clamp a requested output power into a driver's accepted range.
 *
 * Every backend must apply this to the value it stores in its own config
 * before configuring the chip, not just to the value it writes to the chip.
 * Callers persist and report what radio_get_config() returns, so a driver that
 * clamps only on the way to the hardware leaves NVS, the RPC echo, the UI and
 * radio_health all claiming a power the radio was never programmed with.
 *
 * Pure and inline so every backend shares one implementation and one test,
 * including the bare-metal targets that carry no host test suite of their own.
 */
static inline int8_t radio_clamp_tx_power(int8_t power_dbm, int8_t min_dbm, int8_t max_dbm) {
    if (power_dbm > max_dbm)
        return max_dbm;
    if (power_dbm < min_dbm)
        return min_dbm;
    return power_dbm;
}

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

/** The same CAD length as a plain symbol count, for radio chips whose CAD
 *  command takes one directly (LR1110). Deriving it here keeps the
 *  configured CAD length and bramble_cad_timeout_ms's budget in lockstep. */
#define BRAMBLE_CAD_SYMBOL_COUNT (1u << BRAMBLE_CAD_SYMBOL_NUM_REG)

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

/**
 * Consecutive CAD timeouts before radio_cad_check() stops failing open and
 * fails closed (issue #118). Now that #117 sizes the CAD budget correctly, a
 * timeout means the radio did not answer within roughly twice the CAD
 * duration, which points at a stuck SX1262, a wedged DIO1 path or severe SPI
 * starvation. A single timeout is still weak evidence (a one-off missed IRQ
 * under contention), so the first two fail open and transmit anyway; the third
 * fails closed and flags the radio for reinit. Set to 3 to match the driver's
 * existing BUSY_STUCK_THRESHOLD, so BUSY and CAD share one three-strikes trip
 * point, and to bound the blind-transmit (no-LBT) exposure to two frames. */
#define BRAMBLE_CAD_TIMEOUT_REINIT_THRESHOLD 3u

/** Verdict from the CAD-timeout fail-open/closed policy. */
typedef enum {
    CAD_TIMEOUT_FAIL_OPEN,   /* transmit anyway; the radio is probably fine */
    CAD_TIMEOUT_FAIL_CLOSED, /* report busy AND request a radio reinit */
} cad_timeout_action_t;

/** Consecutive-timeout run state for one radio driver. Zero-initialized. */
typedef struct {
    uint8_t consecutive_timeouts;
} cad_timeout_policy_t;

/**
 * Advance the policy on a CAD timeout. Returns CAD_TIMEOUT_FAIL_OPEN for the
 * first BRAMBLE_CAD_TIMEOUT_REINIT_THRESHOLD-1 consecutive timeouts, then
 * CAD_TIMEOUT_FAIL_CLOSED on the threshold-th, resetting the run so recovery
 * starts fresh after the reinit it asks for.
 */
static inline cad_timeout_action_t cad_timeout_policy_on_timeout(cad_timeout_policy_t* p) {
    if (p->consecutive_timeouts < 255u)
        p->consecutive_timeouts++;
    if (p->consecutive_timeouts >= BRAMBLE_CAD_TIMEOUT_REINIT_THRESHOLD) {
        p->consecutive_timeouts = 0;
        return CAD_TIMEOUT_FAIL_CLOSED;
    }
    return CAD_TIMEOUT_FAIL_OPEN;
}

/** Reset the run: any CAD that actually completes clears the timeout streak. */
static inline void cad_timeout_policy_on_success(cad_timeout_policy_t* p) {
    p->consecutive_timeouts = 0;
}

/* Retry cadence for a radio recovery that did not take. The mesh loop calls
 * radio_check_and_clear_reinit() every ~10ms, and a full recovery hard-resets
 * the chip (255ms) and recalibrates, so retrying at loop rate would consume the
 * mesh task. 5s is far below any liveness deadline that matters (a neighbor
 * ages out after minutes) and leaves the loop free in between. */
#define BRAMBLE_RADIO_REINIT_RETRY_MS 5000u

/**
 * Retry state for the driver-owned reinit contract. Zero-initialized.
 *
 * Both backends clear the reinit latch before attempting recovery, so that a
 * hard reset raised DURING the attempt is not swallowed by the attempt that
 * follows it. The cost is that a recovery which fails loses the request
 * entirely: the chip stays at power-on defaults with no RF switch table, no
 * TCXO configuration and no modulation parameters, transmitting nothing any
 * neighbor can hear, while every SPI command still returns success and the
 * mesh task loops on none the wiser. This policy is what makes the backends
 * re-raise instead, on a cadence a genuinely dead chip cannot turn into a
 * busy-loop.
 */
typedef struct {
    bool retry_pending;      /* a previous attempt failed and is owed a retry */
    uint32_t retry_after_ms; /* when that retry becomes due */
} radio_reinit_policy_t;

/**
 * True when a recovery attempt should run now: always, unless a failed
 * attempt's backoff is still in flight. The comparison is a wrap-safe unsigned
 * difference, so it survives the millisecond clock's 49.7-day rollover.
 */
static inline bool radio_reinit_policy_should_attempt(const radio_reinit_policy_t* p,
                                                      uint32_t now_ms) {
    if (!p->retry_pending)
        return true;
    return (uint32_t)(now_ms - p->retry_after_ms) < 0x80000000u;
}

/** Record how an attempt went: success clears the debt, failure schedules the
 *  next attempt one backoff away. */
static inline void radio_reinit_policy_on_result(radio_reinit_policy_t* p, bool ok,
                                                 uint32_t now_ms) {
    if (ok) {
        p->retry_pending = false;
        p->retry_after_ms = 0;
        return;
    }
    p->retry_pending = true;
    p->retry_after_ms = now_ms + BRAMBLE_RADIO_REINIT_RETRY_MS;
}

#endif
