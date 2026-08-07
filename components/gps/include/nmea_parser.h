#ifndef BRAMBLE_NMEA_PARSER_H
#define BRAMBLE_NMEA_PARSER_H

#include <stdbool.h>
#include <stdint.h>

/* Position structure - matches location.h::bramble_position_t */
typedef struct {
    int32_t latitude_e7;  /* degrees * 1e7 */
    int32_t longitude_e7; /* degrees * 1e7 */
    int16_t altitude_m;
    uint8_t accuracy_m;
    uint8_t speed_kmh;
    uint8_t heading_deg2; /* heading / 2 (0-179 = 0-358) */
    uint32_t timestamp;
    bool valid;
    uint8_t sats_used;   /* GGA: satellites used in fix, set even when no fix (0 if unknown) */
    uint8_t fix_quality; /* GGA field 6 digit: 0 invalid, 1 GPS, 2 DGPS, 4/5 RTK, 6 DR */
    uint8_t utc_hour;    /* GGA/RMC UTC hour 0-23, valid only when utc_valid */
    uint8_t utc_min;     /* GGA/RMC UTC minute 0-59, valid only when utc_valid */
    bool utc_valid;      /* true when the UTC time-of-day field was parsed */
} nmea_position_t;

/**
 * Convert NMEA DDMM.MMMM format to decimal degrees.
 * @param field: e.g., "3725.4321" for 37°25.4321'
 * @param dir: 'N', 'S', 'E', or 'W'
 * @return decimal degrees (negative for S/W)
 */
float nmea_dm_to_degrees(const char* field, char dir);

/**
 * Parse NMEA RMC sentence.
 * @param sentence: mutable buffer containing "$GPRMC,..." or "$GNRMC,..."
 * @param pos: output position structure (updated on success)
 * @return true if valid fix parsed, false otherwise
 */
bool nmea_parse_rmc(char* sentence, nmea_position_t* pos);

/**
 * Parse NMEA GGA sentence.
 * @param sentence: mutable buffer containing "$GPGGA,..." or "$GNGGA,..."
 * @param pos: output position structure (updated on success)
 * @return true if valid fix parsed, false otherwise
 */
bool nmea_parse_gga(char* sentence, nmea_position_t* pos);

/* One GSV message's contribution. A GSV cycle is total_msgs messages from one
 * talker; sats_in_view is repeated in every message of the cycle, while
 * tracked/snr_max are per-message and the caller accumulates them. */
typedef struct {
    char talker[3];       /* two-character talker id from the sentence, e.g. "GP" */
    uint8_t total_msgs;   /* field 1, 0 if unparseable */
    uint8_t msg_num;      /* field 2, 0 if unparseable */
    uint8_t sats_in_view; /* field 3, 0-99 */
    uint8_t tracked;      /* satellite groups in THIS message with a nonzero C/N0 */
    uint8_t snr_max;      /* best C/N0 in THIS message in dB-Hz, 0 if none, 0-99 */
} nmea_gsv_t;

/**
 * Parse any NMEA GSV sentence ($GPGSV, $GLGSV, $GAGSV, $GBGSV, $GQGSV, $GNGSV).
 * @param sentence: mutable buffer containing the sentence
 * @param out: filled on success
 * @return true if parsed successfully
 */
bool nmea_parse_gsv(char* sentence, nmea_gsv_t* out);

/**
 * Check whether a raw NMEA line is a $GPTXT/$GNTXT antenna-open warning.
 * Does not tokenize or modify the input.
 * @param sentence: raw sentence
 * @return true if the line reports an open/disconnected antenna
 */
bool nmea_is_antenna_open(const char* sentence);

#endif /* BRAMBLE_NMEA_PARSER_H */
