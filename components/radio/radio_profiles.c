/*
 * Radio profile defaults, split out of radio_esp.c so the table has a single
 * source of truth shared by every target: the SX1262 device driver
 * (radio_esp.c), the virtual emulator driver (radio_virt.c), and host tests.
 * Pure C, no IDF/driver dependencies, so it compiles unchanged on esp32s3,
 * the IDF linux target, and the plain-gcc test harness.
 *
 * These values are the mesh's on-air contract: LONG_RANGE is the default
 * channel profile (SF10/125 kHz/CR4-5), MEDIUM_RANGE the faster short-hop
 * profile (SF7/250 kHz/CR4-5). Changing them changes interoperability with
 * the physical fleet, so they live here and nowhere else.
 */
#include "radio.h"

#include <string.h>

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
