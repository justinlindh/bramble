#include "include/channel_flood.h"
#include "include/discovery.h"

channel_flood_decision_t channel_flood_decide(uint8_t hop_limit, bool is_duplicate,
                                              bool budget_permits, uint32_t random_value) {
    channel_flood_decision_t d = {false, 0, 0};

    if (hop_limit <= 1 || is_duplicate || !budget_permits) {
        return d;
    }

    d.should_relay = true;
    d.new_hop_limit = hop_limit - 1;
    /* Reuse the DES-3 RREQ forward jitter range (discovery.h) rather than a
     * second hardcoded constant set; see channel_flood.h for the rationale. */
    d.jitter_ms = discovery_forward_jitter_ms(random_value);
    return d;
}
