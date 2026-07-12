#include "sas_format.h"

#include <string.h>

void sas_format_grouped(const char sas7[8], char out[9]) {
    if (!out)
        return;
    out[0] = '\0';
    if (!sas7)
        return;

    size_t len = strnlen(sas7, 7);
    if (len != 7)
        return;
    for (size_t i = 0; i < 7; i++) {
        if (sas7[i] < '0' || sas7[i] > '9')
            return;
    }

    out[0] = sas7[0];
    out[1] = sas7[1];
    out[2] = sas7[2];
    out[3] = ' ';
    out[4] = sas7[3];
    out[5] = sas7[4];
    out[6] = sas7[5];
    out[7] = sas7[6];
    out[8] = '\0';
}
