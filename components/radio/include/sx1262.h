#ifndef BRAMBLE_SX1262_H
#define BRAMBLE_SX1262_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>

/* ---------- SX1262 SPI op-codes ---------- */
#define SX1262_CMD_SET_SLEEP 0x84
#define SX1262_CMD_SET_STANDBY 0x80
#define SX1262_CMD_SET_FS 0xC1
#define SX1262_CMD_SET_TX 0x83
#define SX1262_CMD_SET_RX 0x82
#define SX1262_CMD_SET_CAD 0xC5
#define SX1262_CMD_GET_STATUS 0xC0
#define SX1262_CMD_WRITE_REGISTER 0x0D
#define SX1262_CMD_READ_REGISTER 0x1D
#define SX1262_CMD_WRITE_BUFFER 0x0E
#define SX1262_CMD_READ_BUFFER 0x1E
#define SX1262_CMD_SET_DIO_IRQ_PARAMS 0x08
#define SX1262_CMD_GET_IRQ_STATUS 0x12
#define SX1262_CMD_CLR_IRQ_STATUS 0x02
#define SX1262_CMD_SET_PKT_TYPE 0x8A
#define SX1262_CMD_SET_RF_FREQ 0x86
#define SX1262_CMD_SET_PA_CONFIG 0x95
#define SX1262_CMD_SET_TX_PARAMS 0x8E
#define SX1262_CMD_SET_MOD_PARAMS 0x8B
#define SX1262_CMD_SET_PKT_PARAMS 0x8C
#define SX1262_CMD_SET_BUFF_BASE_ADDR 0x8F
#define SX1262_CMD_GET_RX_BUFF_STATUS 0x13
#define SX1262_CMD_GET_PKT_STATUS 0x14
#define SX1262_CMD_SET_DIO3_AS_TCXO 0x97
#define SX1262_CMD_CALIBRATE 0x89
#define SX1262_CMD_CALIBRATE_IMAGE 0x98
#define SX1262_CMD_SET_REGULATOR_MODE 0x96
#define SX1262_CMD_SET_CAD_PARAMS 0x88
#define SX1262_CMD_SET_DIO2_AS_RF_SWITCH 0x9D
#define SX1262_CMD_GET_DEVICE_ERRORS 0x17
#define SX1262_CMD_CLR_DEVICE_ERRORS 0x07

/* ---------- Registers ---------- */

/* PA over-current protection trip point. The reset default suits the low-power
 * PA; the high-power PA needs 140 mA or the PA browns out at full drive. */
#define SX1262_REG_OCP 0x08E7
#define SX1262_OCP_140MA 0x38

/* ---------- IRQ flags ---------- */
#define SX1262_IRQ_TX_DONE (1 << 0)
#define SX1262_IRQ_RX_DONE (1 << 1)
#define SX1262_IRQ_PREAMBLE (1 << 2)
#define SX1262_IRQ_SYNC_WORD (1 << 3)
#define SX1262_IRQ_HEADER_VALID (1 << 4)
#define SX1262_IRQ_HEADER_ERR (1 << 5)
#define SX1262_IRQ_CRC_ERR (1 << 6)
#define SX1262_IRQ_CAD_DONE (1 << 7)
#define SX1262_IRQ_CAD_DETECTED (1 << 8)
#define SX1262_IRQ_TIMEOUT (1 << 9)

/* ---------- Device error flags (GetDeviceErrors) ---------- */

/* The chip latches these until cleared. PA_RAMP is the one that speaks to
 * output power directly: it means the PA did not ramp when a TX was started,
 * so the frame went out at far below the commanded level, or not at all.
 * The calibration and oscillator flags matter too, because an uncalibrated
 * image or an unlocked PLL costs real link budget without failing a TX. */
#define SX1262_DEVERR_RC64K_CALIB (1u << 0)
#define SX1262_DEVERR_RC13M_CALIB (1u << 1)
#define SX1262_DEVERR_PLL_CALIB (1u << 2)
#define SX1262_DEVERR_ADC_CALIB (1u << 3)
#define SX1262_DEVERR_IMG_CALIB (1u << 4)
#define SX1262_DEVERR_XOSC_START (1u << 5)
#define SX1262_DEVERR_PLL_LOCK (1u << 6)
#define SX1262_DEVERR_PA_RAMP (1u << 8)

#define SX1262_DEVERR_ALL                                                                          \
    (SX1262_DEVERR_RC64K_CALIB | SX1262_DEVERR_RC13M_CALIB | SX1262_DEVERR_PLL_CALIB |             \
     SX1262_DEVERR_ADC_CALIB | SX1262_DEVERR_IMG_CALIB | SX1262_DEVERR_XOSC_START |                \
     SX1262_DEVERR_PLL_LOCK | SX1262_DEVERR_PA_RAMP)

/* ---------- Status byte (GetStatus) ---------- */

/* Bits 6:4 chip mode, bits 3:1 command status. */
#define SX1262_STATUS_MODE(s) (((s) >> 4) & 0x07)
#define SX1262_STATUS_CMD(s) (((s) >> 1) & 0x07)

#define SX1262_MODE_STBY_RC 0x02
#define SX1262_MODE_STBY_XOSC 0x03
#define SX1262_MODE_FS 0x04
#define SX1262_MODE_RX 0x05
#define SX1262_MODE_TX 0x06

#define SX1262_CMD_STATUS_DATA_AVAILABLE 0x02
#define SX1262_CMD_STATUS_TIMEOUT 0x03
#define SX1262_CMD_STATUS_PROCESSING_ERR 0x04
#define SX1262_CMD_STATUS_EXEC_FAIL 0x05
#define SX1262_CMD_STATUS_TX_DONE 0x06

static inline const char* sx1262_chip_mode_str(uint8_t status) {
    switch (SX1262_STATUS_MODE(status)) {
    case SX1262_MODE_STBY_RC:
        return "STBY_RC";
    case SX1262_MODE_STBY_XOSC:
        return "STBY_XOSC";
    case SX1262_MODE_FS:
        return "FS";
    case SX1262_MODE_RX:
        return "RX";
    case SX1262_MODE_TX:
        return "TX";
    default:
        return "UNKNOWN";
    }
}

static inline const char* sx1262_cmd_status_str(uint8_t status) {
    switch (SX1262_STATUS_CMD(status)) {
    case SX1262_CMD_STATUS_DATA_AVAILABLE:
        return "data-available";
    case SX1262_CMD_STATUS_TIMEOUT:
        return "timeout";
    case SX1262_CMD_STATUS_PROCESSING_ERR:
        return "processing-error";
    case SX1262_CMD_STATUS_EXEC_FAIL:
        return "exec-failed";
    case SX1262_CMD_STATUS_TX_DONE:
        return "tx-done";
    default:
        return "reserved";
    }
}

/* Buffer size that always holds the full decoded flag list: the eight names
 * plus separators and a NUL. Sized here rather than at each call site so
 * adding a flag is a one-line change. */
#define SX1262_DEVERR_STR_MAX 96

/* Render a device-error bitmask as a space-separated flag list, so a log line
 * names the fault instead of printing a mask a reader has to decode by hand.
 * Always NUL-terminates; writes "none" for a clear mask. Returns buf. */
static inline char* sx1262_device_errors_str(uint16_t errors, char* buf, size_t len) {
    static const struct {
        uint16_t bit;
        const char* name;
    } flags[] = {
        {SX1262_DEVERR_PA_RAMP, "PA_RAMP"},         {SX1262_DEVERR_PLL_LOCK, "PLL_LOCK"},
        {SX1262_DEVERR_XOSC_START, "XOSC_START"},   {SX1262_DEVERR_IMG_CALIB, "IMG_CALIB"},
        {SX1262_DEVERR_ADC_CALIB, "ADC_CALIB"},     {SX1262_DEVERR_PLL_CALIB, "PLL_CALIB"},
        {SX1262_DEVERR_RC13M_CALIB, "RC13M_CALIB"}, {SX1262_DEVERR_RC64K_CALIB, "RC64K_CALIB"},
    };
    if (!buf || len == 0)
        return buf;
    buf[0] = '\0';
    size_t used = 0;
    for (size_t i = 0; i < sizeof(flags) / sizeof(flags[0]); i++) {
        if (!(errors & flags[i].bit))
            continue;
        int n = snprintf(buf + used, len - used, "%s%s", used ? " " : "", flags[i].name);
        if (n < 0 || (size_t)n >= len - used) {
            buf[used] = '\0'; /* would not fit: stop rather than truncate mid-name */
            break;
        }
        used += (size_t)n;
    }
    if (used == 0)
        snprintf(buf, len, "none");
    return buf;
}

/* ---------- Output power ---------- */

/* SetTxParams accepts -9..+22 dBm on the SX1262 high-power PA. Values outside
 * that range are not defined by the part, so they must never reach the chip:
 * the regional plan's ceiling is a regulatory limit (30 dBm in US915/AU915),
 * not a hardware one, and clamping to it alone lets an out-of-range value
 * through. */
#define SX1262_TX_POWER_MIN_DBM (-9)
#define SX1262_TX_POWER_MAX_DBM 22

static inline int8_t sx1262_clamp_tx_power(int8_t power_dbm) {
    if (power_dbm > SX1262_TX_POWER_MAX_DBM)
        return SX1262_TX_POWER_MAX_DBM;
    if (power_dbm < SX1262_TX_POWER_MIN_DBM)
        return SX1262_TX_POWER_MIN_DBM;
    return power_dbm;
}

/* A SetPaConfig operating point: the PA duty cycle and high-power-max pair,
 * plus the output level that pair is characterized for. */
typedef struct {
    uint8_t pa_duty_cycle;
    uint8_t hp_max;
    int8_t rated_dbm;
} sx1262_pa_op_point_t;

/* Pick the PA operating point to bias the high-power PA for a requested level.
 *
 * The datasheet characterizes four points for the SX1262 (+14, +17, +20, +22
 * dBm), each a (paDutyCycle, hpMax) pair. We select the lowest characterized
 * point that still covers the request, so the PA always has the headroom to
 * reach the commanded level, and leave SetTxParams as the continuous trim.
 * Biasing the PA for +22 at every level (what a single hardcoded pair does)
 * still transmits, but burns full PA current to produce an attenuated signal.
 *
 * Requests are assumed already clamped to the chip range; anything above the
 * top point maps to it, and anything below the bottom point uses the bottom
 * point's bias with SetTxParams carrying the rest of the reduction. */
static inline sx1262_pa_op_point_t sx1262_pa_op_point_for(int8_t power_dbm) {
    if (power_dbm > 20) {
        sx1262_pa_op_point_t p = {0x04, 0x07, 22};
        return p;
    }
    if (power_dbm > 17) {
        sx1262_pa_op_point_t p = {0x03, 0x05, 20};
        return p;
    }
    if (power_dbm > 14) {
        sx1262_pa_op_point_t p = {0x02, 0x03, 17};
        return p;
    }
    sx1262_pa_op_point_t p = {0x02, 0x02, 14};
    return p;
}

/* ---------- Error codes ---------- */

/* Generic failure: the command did not reach the chip, or SPI errored. */
#define SX1262_ERR_FAIL (-1)

/* The BUSY line was stuck long enough that the driver hard-reset the chip.
 * The command was NOT issued and the SX1262 now sits in power-on defaults:
 * no TCXO, no calibration, no packet type, no sync word. Callers must abort
 * the operation in progress rather than retry, and let the reinit path
 * (sx1262_needs_reinit / radio_check_and_clear_reinit) reconfigure the chip
 * before any further commands are issued. */
#define SX1262_ERR_RESET (-2)

/* Map a LoRa bandwidth in Hz to the SX1262 SetModulationParams register code.
 * 125 kHz -> 0x04, 250 kHz -> 0x05, 500 kHz -> 0x06. The firmware only offers
 * these three bandwidths; anything above 250 kHz maps to the 500 kHz code.
 *
 * This is the single source of truth for the mapping. It is deliberately a
 * pure inline in the header so it is host-testable without the SPI driver: the
 * previous kHz-through-uint8_t path truncated 500 to 244 and silently ran the
 * radio at 125 kHz (issue #149). */
static inline uint8_t sx1262_bw_reg_from_hz(uint32_t bw_hz) {
    if (bw_hz <= 125000)
        return 0x04;
    if (bw_hz <= 250000)
        return 0x05;
    return 0x06;
}

/* ---------- Functions ---------- */

/* Lifecycle */
int sx1262_init(void);
bool sx1262_needs_reinit(void);
void sx1262_clear_reinit(void);
void sx1262_request_reinit(void);

/* Low-level SPI */
int sx1262_write_register(uint16_t addr, const uint8_t* data, size_t len);
int sx1262_read_register(uint16_t addr, uint8_t* data, size_t len);
int sx1262_write_buffer(uint8_t offset, const uint8_t* data, size_t len);
int sx1262_read_buffer(uint8_t offset, uint8_t* data, size_t len);

/* Readback. SetTxParams and SetPaConfig are write-only op-codes, so the
 * commanded output power can never be read back from the part and no register
 * reports actual radiated power. These three are what the chip will admit to:
 * the latched device errors, the chip mode plus last-command status, and the
 * OCP register, which is the one PA setting that does read back and so proves
 * config writes are landing at all. */
int sx1262_get_status(uint8_t* status);
int sx1262_get_device_errors(uint16_t* errors);
int sx1262_clear_device_errors(void);

/* Configuration */
int sx1262_set_standby(uint8_t mode);
int sx1262_set_rf_frequency(float freq_mhz);
int sx1262_set_pa_config(int8_t power_dbm);
int sx1262_set_tx_params(int8_t power_dbm, uint8_t ramp_time);
int sx1262_set_modulation_params(uint8_t sf, uint32_t bw_hz, uint8_t cr, uint8_t ldro);
int sx1262_set_packet_params(uint16_t preamble, uint8_t header_type, uint8_t payload_len,
                             uint8_t crc_on, uint8_t invert_iq);
int sx1262_set_dio_irq_params(uint16_t irq_mask, uint16_t dio1_mask, uint16_t dio2_mask,
                              uint16_t dio3_mask);
int sx1262_clear_irq_status(uint16_t mask);
uint16_t sx1262_get_irq_status(void);

/* TX / RX */
int sx1262_set_tx(uint32_t timeout_ms);
int sx1262_set_rx(uint32_t timeout_ms);
int sx1262_set_cad(void);
int sx1262_set_cad_params(uint8_t symbol_num, uint8_t det_peak, uint8_t det_min, uint8_t exit_mode,
                          uint32_t timeout);
int sx1262_get_rx_buffer_status(uint8_t* payload_len, uint8_t* rx_start_offset);
int sx1262_get_packet_status(int16_t* rssi, int8_t* snr);
int sx1262_set_sleep(uint8_t config);

/* Heltec V3 specific */
int sx1262_calibrate_image(float freq_mhz);
int sx1262_set_dio2_as_rf_switch(bool enable);

#endif /* BRAMBLE_SX1262_H */
