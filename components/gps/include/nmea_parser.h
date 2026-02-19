#ifndef BRAMBLE_NMEA_PARSER_H
#define BRAMBLE_NMEA_PARSER_H

#include <stdbool.h>
#include <stdint.h>

/* Position structure - matches location.h::bramble_position_t */
typedef struct {
    int32_t latitude_e7;   /* degrees * 1e7 */
    int32_t longitude_e7;  /* degrees * 1e7 */
    int16_t altitude_m;
    uint8_t accuracy_m;
    uint8_t speed_kmh;
    uint8_t heading_deg2;  /* heading / 2 (0-179 = 0-358) */
    uint32_t timestamp;
    bool valid;
} nmea_position_t;

/**
 * Convert NMEA DDMM.MMMM format to decimal degrees.
 * @param field: e.g., "3725.4321" for 37°25.4321'
 * @param dir: 'N', 'S', 'E', or 'W'
 * @return decimal degrees (negative for S/W)
 */
float nmea_dm_to_degrees(const char *field, char dir);

/**
 * Parse NMEA RMC sentence.
 * @param sentence: mutable buffer containing "$GPRMC,..." or "$GNRMC,..."
 * @param pos: output position structure (updated on success)
 * @return true if valid fix parsed, false otherwise
 */
bool nmea_parse_rmc(char *sentence, nmea_position_t *pos);

/**
 * Parse NMEA GGA sentence.
 * @param sentence: mutable buffer containing "$GPGGA,..." or "$GNGGA,..."
 * @param pos: output position structure (updated on success)
 * @return true if valid fix parsed, false otherwise
 */
bool nmea_parse_gga(char *sentence, nmea_position_t *pos);

#endif /* BRAMBLE_NMEA_PARSER_H */
