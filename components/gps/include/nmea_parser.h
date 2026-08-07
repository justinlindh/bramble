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
    uint8_t sats_used; /* GGA: satellites used in fix, set even when no fix (0 if unknown) */
    uint8_t utc_hour;  /* GGA UTC hour 0-23, valid only when utc_valid */
    uint8_t utc_min;   /* GGA UTC minute 0-59, valid only when utc_valid */
    bool utc_valid;    /* true when the UTC time-of-day field was parsed */
    /* RMC carries the UTC date, which GGA does not. The date is what makes a
     * daylight-saving rule evaluable, so local-time rendering needs it in
     * addition to the time of day. */
    uint16_t utc_year;   /* full year, e.g. 2026, valid only when utc_date_valid */
    uint8_t utc_month;   /* 1-12, valid only when utc_date_valid */
    uint8_t utc_day;     /* 1-31, valid only when utc_date_valid */
    bool utc_date_valid; /* true when the RMC date field was parsed */
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

/**
 * Parse NMEA GSV sentence for total satellites in view.
 * @param sentence: mutable buffer containing "$GPGSV,..." or "$GNGSV,..."
 * @param sats_in_view: output, set to the total-satellites-in-view field
 * @return true if parsed successfully
 */
bool nmea_parse_gsv(char* sentence, uint8_t* sats_in_view);

/**
 * Check whether a raw NMEA line is a $GPTXT/$GNTXT antenna-open warning.
 * Does not tokenize or modify the input.
 * @param sentence: raw sentence
 * @return true if the line reports an open/disconnected antenna
 */
bool nmea_is_antenna_open(const char* sentence);

#endif /* BRAMBLE_NMEA_PARSER_H */
