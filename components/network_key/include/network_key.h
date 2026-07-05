#ifndef BRAMBLE_NETWORK_KEY_H
#define BRAMBLE_NETWORK_KEY_H
#include <stdint.h>
#include <stddef.h>

/*
 * Fail-closed control-plane network key provider (mandatory-provisioning
 * campaign). An UNPROVISIONED node has NO usable network key: there is no
 * public-PSK fallback. network_key_get() fails, network_key_mac() emits no
 * valid MAC, and network_key_fingerprint() reports the all-zero sentinel.
 * A node becomes provisioned only when an operator sets a real key via
 * network_key_set_provisioned (the setNetworkKey RPC).
 *
 * This component is the fail-closed FOUNDATION; it does not on its own make
 * an unprovisioned node inert. The control-plane call sites that build MACs
 * (routing_auth, discovery, mesh_task) must check network_key_mac()'s return
 * value and refuse to originate/verify when it is nonzero: that hardening is
 * the campaign's Task 2.
 */

/* Marks the node as provisioned with a real, non-public network key. */
void network_key_set_provisioned(const uint8_t key[32]);

/* Reverts to unprovisioned. After this, network_key_get() fails closed until
 * a key is set again. */
void network_key_clear(void);

/* Copies the provisioned key into key_out and returns 0 IFF provisioned.
 * When unprovisioned, returns nonzero and writes NOTHING to key_out
 * (fail-closed: there is no fallback key material). */
int network_key_get(uint8_t key_out[32]);

/* 1 if a real key is currently provisioned (set since boot and not since
 * network_key_clear'd), 0 otherwise. */
int network_key_is_provisioned(void);

/*
 * Fail-closed domain-separated control-plane MAC: HMAC(network_key,
 * label || data)[0:8]. Returns 0 and writes the MAC to out when provisioned.
 * When UNPROVISIONED, returns nonzero and writes the all-zero sentinel to out
 * (it never computes an HMAC over a fallback or zeroed key). Callers MUST
 * check the return value: on nonzero they must refuse to originate or verify
 * the message rather than trust the sentinel bytes (Task 2 hardening). The
 * per-type label (e.g. "bramble-rrep-v2") domain-separates message types.
 * len is the data length only (not the label); an assert bounds it.
 */
int network_key_mac(const char* label, const uint8_t* data, size_t len, uint8_t out[8]);

/*
 * One-way fingerprint of the current network key: SHA256(key)[0:4] when
 * provisioned. Safe to expose (does not reveal the key); identical on nodes
 * that share a key, so an operator can confirm a fleet converged without
 * transmitting the secret. When UNPROVISIONED, writes the all-zero sentinel
 * (0x00000000), which explicitly means "no key provisioned".
 */
void network_key_fingerprint(uint8_t out[4]);

#endif
