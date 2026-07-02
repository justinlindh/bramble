#include "crypto_entropy.h"
#include <string.h>

/* Platform-independent so it compiles into both the device and host builds.
 * Plain bool, no lock: main() flips it at well-defined boot/RF-transition
 * points on CPU0, while the mesh task (pinned to CPU1) only reads it. */
static bool s_entropy_ready = false;

void crypto_entropy_set_ready(bool ready) { s_entropy_ready = ready; }
bool crypto_entropy_is_ready(void) { return s_entropy_ready; }

int crypto_entropy_fill(uint8_t* buf, size_t len, uint32_t (*source)(void)) {
    if (!s_entropy_ready) {
        /* Fail closed: no entropy source seeded yet. Zero the buffer so a caller
         * that ignores the -1 cannot install predictable key bytes (SEC-L1). */
        memset(buf, 0, len);
        return -1;
    }
    for (size_t i = 0; i < len; i += 4) {
        uint32_t r = source();
        size_t remaining = len - i;
        size_t to_copy = remaining < 4 ? remaining : 4;
        memcpy(buf + i, &r, to_copy);
    }
    return 0;
}
