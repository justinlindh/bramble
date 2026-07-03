#ifndef BRAMBLE_NETWORK_KEY_H
#define BRAMBLE_NETWORK_KEY_H
#include <stdint.h>
#include <stddef.h>

/*
 * Task 3.1 (PART 3, control-plane authentication): the minimal network key
 * provider. This is STAGED INFRASTRUCTURE, not a closed finding. It supplies
 * the key and MAC primitive that RREP/RERR/ACK/delivery-receipt/beacon
 * verification will build on, but the shipped default is UNPROVISIONED:
 * network_key_get() derives a fallback key from BRAMBLE_PUBLIC_CHANNEL_PSK,
 * a PUBLIC compile-time constant checked into this repository. Anyone who
 * reads that constant derives the identical fallback key and can forge a
 * valid MAC for any label this component knows about. Do NOT describe this
 * component, on its own, as closing SEC-H1, SEC-H2, NEW-SEC-4, or
 * NEW-SEC-8: those findings stay open until real per-fleet key
 * provisioning lands (a separate workstream; distribution UX is an open
 * question, not addressed here).
 */

/* Marks the node as provisioned with a real, non-public network key
 * (e.g. loaded from NVS at boot, or set via the setNetworkKey RPC). */
void network_key_set_provisioned(const uint8_t key[32]);

/* Reverts to unprovisioned: network_key_get() falls back to the
 * PSK-derived key again. */
void network_key_clear(void);

/* Fills key_out with the provisioned key if network_key_is_provisioned(),
 * else derives and returns the PSK fallback (see the staging note above).
 * Returns 0 on success, matching this codebase's other crypto primitives'
 * convention; the only failure mode is an internal HKDF size error, which
 * cannot happen with these fixed-size arguments. */
int network_key_get(uint8_t key_out[32]);

/* 1 if a real key has been set via network_key_set_provisioned since boot
 * (or since the last network_key_clear), 0 if still on the PSK fallback. */
int network_key_is_provisioned(void);

/*
 * Domain-separated control-plane MAC: HMAC(network_key, label || data)[0:8].
 * The per-type label (e.g. "bramble-rrep-v2") prevents a MAC computed for
 * one message type from being replayed as if it authenticated a different
 * type, even when the underlying bytes happen to coincide. len is the
 * caller's data length only (not including the label); callers are trusted
 * internal code building a MAC over a message body they just serialized,
 * so an assert bounds it defensively rather than failing at runtime.
 */
void network_key_mac(const char* label, const uint8_t* data, size_t len, uint8_t out[8]);

/*
 * One-way fingerprint of the current network key: SHA256(network_key_get())[0:4].
 * Safe to expose (does not reveal the key); identical on nodes that share a
 * key, so an operator can confirm a fleet converged on one network key
 * without transmitting the secret. Returns the fallback key's fingerprint
 * when unprovisioned (a known value that marks "still on the public default").
 */
void network_key_fingerprint(uint8_t out[4]);

#endif
