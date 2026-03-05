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
