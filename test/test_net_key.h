#ifndef BRAMBLE_TEST_NET_KEY_H
#define BRAMBLE_TEST_NET_KEY_H

#include "network_key.h"
#include <stdint.h>

/*
 * Shared fixed 32-byte network key for the control-plane auth host suites.
 *
 * Mandatory-provisioning (Task 2): the network key is now mandatory and an
 * UNPROVISIONED node is inert -- its *_sign emits the all-zero sentinel and
 * its *_verify rejects before comparing. These suites exercise the sign/verify
 * round trip and forgery rejection of a PROVISIONED node, so each setUp()
 * provisions this fixed key (the pre-campaign public-PSK fallback that made
 * an unprovisioned node "just work" is gone). The value is arbitrary but
 * fixed and recognizable so a fleet-wide shared key is modeled faithfully.
 */
static const uint8_t BRAMBLE_TEST_NET_KEY[32] = {
    0xC0, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xCB, 0xCC, 0xCD, 0xCE, 0xCF,
    0xD0, 0xD1, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xDB, 0xDC, 0xDD, 0xDE, 0xDF};

static inline void bramble_test_provision_net_key(void) {
    network_key_set_provisioned(BRAMBLE_TEST_NET_KEY);
}

#endif /* BRAMBLE_TEST_NET_KEY_H */
