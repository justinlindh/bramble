#ifndef BRAMBLE_SX1262_H
#define BRAMBLE_SX1262_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

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
int sx1262_reset(void);
void sx1262_hard_reset(void);
bool sx1262_needs_reinit(void);
void sx1262_clear_reinit(void);
void sx1262_request_reinit(void);
int sx1262_wait_busy(uint32_t timeout_ms);

/* Low-level SPI */
int sx1262_write_command(uint8_t cmd, const uint8_t* data, size_t len);
int sx1262_read_command(uint8_t cmd, uint8_t* data, size_t len);
int sx1262_write_register(uint16_t addr, const uint8_t* data, size_t len);
int sx1262_write_buffer(uint8_t offset, const uint8_t* data, size_t len);
int sx1262_read_buffer(uint8_t offset, uint8_t* data, size_t len);

/* Configuration */
int sx1262_set_standby(uint8_t mode);
int sx1262_set_packet_type(uint8_t type);
int sx1262_set_rf_frequency(float freq_mhz);
int sx1262_set_pa_config(int8_t power_dbm);
int sx1262_set_tx_params(int8_t power_dbm, uint8_t ramp_time);
int sx1262_set_modulation_params(uint8_t sf, uint32_t bw_hz, uint8_t cr, uint8_t ldro);
int sx1262_set_packet_params(uint16_t preamble, uint8_t header_type, uint8_t payload_len,
                             uint8_t crc_on, uint8_t invert_iq);
int sx1262_set_buffer_base_address(uint8_t tx_base, uint8_t rx_base);
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
int sx1262_set_dio3_as_tcxo(float voltage, uint32_t timeout_ms);
int sx1262_calibrate(uint8_t cal_mask);
int sx1262_calibrate_image(float freq_mhz);
int sx1262_set_regulator_mode(uint8_t mode);
int sx1262_set_dio2_as_rf_switch(bool enable);

#endif /* BRAMBLE_SX1262_H */
