#ifndef SAS_FORMAT_H
#define SAS_FORMAT_H

#include <stddef.h>

/*
 * Pure formatting helper for the pager SAS verification screen (Task 8):
 * groups a 7-digit identity SAS ("1234567") into the read-aloud form
 * ("123 4567"). Host-testable on its own, unlike the LVGL screen that uses
 * it. out must be at least 9 bytes (8 chars + NUL). A NULL/short/malformed
 * sas7 is handled safely: out is left an empty string.
 */
void sas_format_grouped(const char sas7[8], char out[9]);

#endif
