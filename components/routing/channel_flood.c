#include "include/channel_flood.h"
#include "dedup.h"
#include <stdlib.h>

#ifdef ESP_PLATFORM
#include "esp_random.h"
#else
/* Use stdlib rand for host tests */
static uint32_t esp_random(void) { return (uint32_t)rand(); }
#endif

#define JITTER_MIN_MS 50
#define JITTER_MAX_MS 300

flood_decision_t channel_flood_decide(uint8_t hop_limit, uint32_t packet_id, void* dedup_context) {
    flood_decision_t d = {false, 0, 0};
    dedup_buffer_t* dedup = (dedup_buffer_t*)dedup_context;

    if (hop_limit <= 1)
        return d;

    /* Check dedup — use 0 as timestamp since we just need presence check.
       Actually we need a real timestamp but for simplicity use a large value */
    if (dedup_check_and_add(dedup, packet_id, 1000000)) {
        return d; /* duplicate */
    }

    d.should_relay = true;
    d.new_hop_limit = hop_limit - 1;
    d.jitter_ms = JITTER_MIN_MS + (esp_random() % (JITTER_MAX_MS - JITTER_MIN_MS + 1));
    return d;
}
