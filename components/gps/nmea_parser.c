#include "nmea_parser.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <time.h>
#include <ctype.h>

/* Helper: check if field is empty */
static bool field_empty(const char* field) { return !field || field[0] == '\0'; }

/*
 * Split an NMEA sentence into comma-separated fields IN PLACE, preserving
 * empty fields. NMEA uses empty fields to mean "no data" (e.g. an RMC with
 * no speed/track: ",,", or a no-fix GGA with empty lat/lon), and their
 * position is significant. strtok(",*") collapses runs of delimiters, so it
 * silently drops empties and shifts every later field left -- making e.g.
 * RMC speed read from the date field or GGA sats-used read from HDOP. This
 * scanner instead treats each comma as a field boundary and stops at the '*'
 * checksum delimiter (whose trailing hex is not a data field).
 *
 * Writes NULs over the delimiters, stores up to max_fields field pointers,
 * and returns the number stored (capped at max_fields), matching the
 * bounded field_count the callers already reason about.
 */
static int nmea_split(char* sentence, char** fields, int max_fields) {
    int count = 0;
    if (max_fields <= 0)
        return 0;

    fields[count++] = sentence;
    for (char* p = sentence; *p; p++) {
        if (*p == '*') {
            *p = '\0';
            break;
        }
        if (*p == ',') {
            *p = '\0';
            if (count < max_fields) {
                fields[count] = p + 1;
            }
            count++;
        }
    }
    return count < max_fields ? count : max_fields;
}

static bool valid_hemisphere(char dir, bool latitude) {
    if (latitude) {
        return dir == 'N' || dir == 'S';
    }
    return dir == 'E' || dir == 'W';
}

static bool validate_coord_field(const char* field, bool latitude) {
    if (field_empty(field))
        return false;

    const size_t len = strlen(field);
    const size_t min_digits_before_dot = latitude ? 4U : 5U;

    const char* dot = strchr(field, '.');
    if (!dot)
        return false;

    size_t digits_before_dot = (size_t)(dot - field);
    if (digits_before_dot < min_digits_before_dot)
        return false;

    for (size_t i = 0; i < len; i++) {
        char c = field[i];
        if (c == '.')
            continue;
        if (!isdigit((unsigned char)c))
            return false;
    }

    float raw = atof(field);
    int degrees = (int)(raw / 100.0f);
    float minutes = raw - (degrees * 100.0f);

    if (minutes < 0.0f || minutes >= 60.0f)
        return false;
    if (latitude && degrees > 90)
        return false;
    if (!latitude && degrees > 180)
        return false;

    return true;
}

/* Convert NMEA DDMM.MMMM to decimal degrees */
float nmea_dm_to_degrees(const char* field, char dir) {
    if (field_empty(field))
        return 0.0f;

    /* Parse field like "3725.4321" or "12215.6789" */
    float raw = atof(field);
    int degrees = (int)(raw / 100.0f);
    float minutes = raw - (degrees * 100.0f);
    float decimal = degrees + (minutes / 60.0f);

    /* Apply hemisphere direction */
    if (dir == 'S' || dir == 'W') {
        decimal = -decimal;
    }

    return decimal;
}

/* Parse $GPRMC or $GNRMC sentence */
bool nmea_parse_rmc(char* sentence, nmea_position_t* pos) {
    if (!sentence || !pos)
        return false;

    /* Example: $GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A
     * Fields: 0=GPRMC, 1=time, 2=status, 3=lat, 4=N/S, 5=lon, 6=E/W,
     *         7=speed_knots, 8=track_deg, 9=date, 10=mag_var, 11=E/W, 12=checksum */

    char* fields[13];
    int field_count = nmea_split(sentence, fields, 13);

    /* Need at least sentence type through longitude direction */
    if (field_count < 7)
        return false;

    /* Check sentence type */
    if (strcmp(fields[0], "$GPRMC") != 0 && strcmp(fields[0], "$GNRMC") != 0) {
        return false;
    }

    /* Check status field (A=valid, V=invalid) */
    if (field_count < 3 || fields[2][0] != 'A') {
        pos->valid = false;
        return false;
    }

    if (!valid_hemisphere(fields[4][0], true) || !valid_hemisphere(fields[6][0], false)) {
        pos->valid = false;
        return false;
    }

    if (!validate_coord_field(fields[3], true) || !validate_coord_field(fields[5], false)) {
        pos->valid = false;
        return false;
    }

    /* Parse latitude */
    float lat = nmea_dm_to_degrees(fields[3], fields[4][0]);
    pos->latitude_e7 = (int32_t)(lat * 1e7f);

    /* Parse longitude */
    float lon = nmea_dm_to_degrees(fields[5], fields[6][0]);
    pos->longitude_e7 = (int32_t)(lon * 1e7f);

    /* Parse speed (convert knots to km/h) */
    if (field_count > 7 && !field_empty(fields[7])) {
        float speed_knots = atof(fields[7]);
        float speed_kmh = speed_knots * 1.852f;
        pos->speed_kmh = (speed_kmh > 255.0f) ? 255 : (uint8_t)speed_kmh;
    }

    /* Parse heading */
    if (field_count > 8 && !field_empty(fields[8])) {
        float heading = atof(fields[8]);
        pos->heading_deg2 = (uint8_t)(heading / 2.0f);
        if (pos->heading_deg2 > 179)
            pos->heading_deg2 = 179;
    }

    pos->valid = true;
    return true;
}

/* Parse $GPGSV or $GNGSV sentence for total satellites in view.
 * Example: $GPGSV,3,1,11,10,63,137,17,07,61,308,17,05,59,169,18,30,54,042,*7D
 * Fields: 0=GPGSV, 1=total_msgs, 2=msg_num, 3=sats_in_view, 4+=per-satellite groups. */
bool nmea_parse_gsv(char* sentence, uint8_t* sats_in_view) {
    if (!sentence || !sats_in_view)
        return false;

    char* fields[8];
    int field_count = nmea_split(sentence, fields, 8);

    if (field_count < 4)
        return false;

    if (strcmp(fields[0], "$GPGSV") != 0 && strcmp(fields[0], "$GNGSV") != 0) {
        return false;
    }

    if (field_empty(fields[3]))
        return false;

    int sats = atoi(fields[3]);
    if (sats < 0)
        sats = 0;
    if (sats > 99)
        sats = 99;

    *sats_in_view = (uint8_t)sats;
    return true;
}

/* $GPTXT/$GNTXT carries free-form receiver diagnostics, e.g.
 * "$GPTXT,01,01,02,ANTENNA OPEN*35". Match on the raw line rather than
 * tokenizing, since the text field's content isn't otherwise structured. */
bool nmea_is_antenna_open(const char* sentence) {
    if (!sentence)
        return false;
    if (strncmp(sentence, "$GPTXT", 6) != 0 && strncmp(sentence, "$GNTXT", 6) != 0) {
        return false;
    }
    return strstr(sentence, "ANTENNA OPEN") != NULL;
}

/* Parse $GPGGA or $GNGGA sentence */
bool nmea_parse_gga(char* sentence, nmea_position_t* pos) {
    if (!sentence || !pos)
        return false;

    /* Example: $GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47
     * Fields: 0=GPGGA, 1=time, 2=lat, 3=N/S, 4=lon, 5=E/W, 6=fix_quality,
     *         7=satellites, 8=hdop, 9=altitude, 10=M, 11=geoid, 12=M, ... */

    char* fields[15];
    int field_count = nmea_split(sentence, fields, 15);

    /* Need at least through fix quality */
    if (field_count < 7)
        return false;

    /* Check sentence type */
    if (strcmp(fields[0], "$GPGGA") != 0 && strcmp(fields[0], "$GNGGA") != 0) {
        return false;
    }

    /* UTC time-of-day from field 1 ("hhmmss" or "hhmmss.sss"). Only HH:MM is
     * needed for the status-bar clock, and a valid fix guarantees this field is
     * present. The caller persists this only alongside a valid fix. */
    pos->utc_valid = false;
    if (field_count > 1 && !field_empty(fields[1]) && isdigit((unsigned char)fields[1][0]) &&
        isdigit((unsigned char)fields[1][1]) && isdigit((unsigned char)fields[1][2]) &&
        isdigit((unsigned char)fields[1][3])) {
        int hh = (fields[1][0] - '0') * 10 + (fields[1][1] - '0');
        int mm = (fields[1][2] - '0') * 10 + (fields[1][3] - '0');
        if (hh < 24 && mm < 60) {
            pos->utc_hour = (uint8_t)hh;
            pos->utc_min = (uint8_t)mm;
            pos->utc_valid = true;
        }
    }

    /* Satellites-used is reported even without a fix (useful for a "searching,
     * N sats" status), so capture it before the fix-quality gate below. */
    if (field_count > 7 && !field_empty(fields[7])) {
        int sats = atoi(fields[7]);
        if (sats < 0)
            sats = 0;
        if (sats > 99)
            sats = 99;
        pos->sats_used = (uint8_t)sats;
    } else {
        pos->sats_used = 0;
    }

    /* Check fix quality (0=invalid, 1=GPS, 2=DGPS, etc.) */
    if (field_empty(fields[6]) || fields[6][0] == '0') {
        pos->valid = false;
        return false;
    }

    if (!valid_hemisphere(fields[3][0], true) || !valid_hemisphere(fields[5][0], false)) {
        pos->valid = false;
        return false;
    }

    if (!validate_coord_field(fields[2], true) || !validate_coord_field(fields[4], false)) {
        pos->valid = false;
        return false;
    }

    /* Parse latitude */
    float lat = nmea_dm_to_degrees(fields[2], fields[3][0]);
    pos->latitude_e7 = (int32_t)(lat * 1e7f);

    /* Parse longitude */
    float lon = nmea_dm_to_degrees(fields[4], fields[5][0]);
    pos->longitude_e7 = (int32_t)(lon * 1e7f);

    /* Parse altitude */
    if (field_count > 9 && !field_empty(fields[9])) {
        float alt = atof(fields[9]);
        pos->altitude_m = (int16_t)alt;
    }

    /* Parse HDOP for accuracy estimate (HDOP * 5 meters is a rough estimate) */
    if (field_count > 8 && !field_empty(fields[8])) {
        float hdop = atof(fields[8]);
        float acc = hdop * 5.0f;
        pos->accuracy_m = (acc > 255.0f) ? 255 : (uint8_t)acc;
    }

    pos->valid = true;
    return true;
}
