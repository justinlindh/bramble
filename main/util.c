#include "util.h"

#include <inttypes.h>
#include <stdio.h>

const char* addr_hex(uint32_t addr, char* buf, size_t len) {
    snprintf(buf, len, "%08" PRIX32, addr);
    return buf;
}
