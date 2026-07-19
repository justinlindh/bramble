#include "radio.h"
#include <math.h>

uint32_t bramble_calculate_airtime_us(uint16_t payload_bytes, uint8_t sf, uint32_t bw_hz,
                                      uint8_t cr) {
    /*
     * LoRa airtime calculation per Semtech AN1200.13.
     * cr: coding rate 1..4 (1 = 4/5, 2 = 4/6, 3 = 4/7, 4 = 4/8)
     * Assumes CRC enabled, explicit header mode.
     */
    double bw = (double)bw_hz;
    double t_sym = (double)(1u << sf) / bw; /* seconds per symbol */

    /* Preamble duration: programmed preamble + 4.25 sync symbols */
    double n_preamble = (sf >= 9) ? 12.0 : 8.0;
    double t_preamble = (n_preamble + 4.25) * t_sym;

    /* Low data rate optimization: enabled for SF11/SF12 at 125kHz */
    int de = (sf >= 11 && bw_hz <= 125000) ? 1 : 0;

    /* Explicit header (H=0), CRC enabled */
    int ih = 0;
    double num = 8.0 * (double)payload_bytes - 4.0 * (double)sf + 28.0 + 16.0 - 20.0 * (double)ih;
    double den = 4.0 * ((double)sf - 2.0 * (double)de);
    double n_payload = 8.0 + fmax(ceil(num / den) * (double)(cr + 4), 0.0);

    double t_payload = n_payload * t_sym;
    double t_total = t_preamble + t_payload;

    return (uint32_t)(t_total * 1e6 + 0.5);
}

uint32_t bramble_symbol_time_us(uint8_t sf, uint32_t bw_hz) {
    if (sf < 5)
        sf = 5;
    if (sf > 12)
        sf = 12;
    if (bw_hz == 0)
        bw_hz = 125000;
    /* (2^sf / bw_hz) seconds, expressed in microseconds and rounded up so a
     * truncated symbol never makes the derived timeout too tight. */
    uint64_t num = (uint64_t)(1u << sf) * 1000000ull;
    return (uint32_t)((num + bw_hz - 1) / bw_hz);
}

uint32_t bramble_cad_timeout_ms(uint8_t sf, uint32_t bw_hz, uint8_t cad_symbol_num_reg) {
    if (cad_symbol_num_reg > 4)
        cad_symbol_num_reg = 4; /* SX1262 caps the field at 16 symbols */
    uint32_t symbols = 1u << cad_symbol_num_reg;
    uint64_t cad_us = (uint64_t)symbols * bramble_symbol_time_us(sf, bw_hz);

    /* 100% proportional margin: CadDone lands after the sampled symbols plus
     * the chip's own detection processing, and the caller may be preempted or
     * queued behind an SPI transfer to a shared-bus display. */
    uint64_t budget_us = cad_us * 2u;
    uint32_t budget_ms = (uint32_t)((budget_us + 999u) / 1000u) + BRAMBLE_CAD_OVERHEAD_MS;

    return (budget_ms < BRAMBLE_CAD_TIMEOUT_MIN_MS) ? BRAMBLE_CAD_TIMEOUT_MIN_MS : budget_ms;
}
