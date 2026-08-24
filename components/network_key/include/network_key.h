#ifndef BRAMBLE_NETWORK_KEY_H
#define BRAMBLE_NETWORK_KEY_H
#include <stdint.h>
#include <stddef.h>

/*
 * Fail-closed control-plane network key provider.
 * An UNPROVISIONED node has NO usable network key: there is no
 * public-PSK fallback. network_key_get() fails, network_key_mac() emits no
 * valid MAC, and network_key_fingerprint() reports the all-zero sentinel.
 * A node becomes provisioned only by generating a fresh key
 * (network_key_generate_provision), loading a persisted one
 * (network_key_load_from_nvs), or having an operator set one
 * (network_key_set_provisioned via the setNetworkKey RPC). The key is
 * persisted to NVS on the set and generate paths so it survives reboot.
 *
 * This component is the fail-closed FOUNDATION; it does not on its own make
 * an unprovisioned node inert. The control-plane call sites that build MACs
 * (routing_auth, discovery, mesh_task) must check network_key_mac()'s return
 * value and refuse to originate/verify when it is nonzero.
 */

/* Marks the node as provisioned with a real, non-public network key and
 * persists it to non-volatile storage (device NVS; in-memory on host) so it
 * survives reboot. Called by generate_provision and the setNetworkKey RPC. */
void network_key_set_provisioned(const uint8_t key[32]);

/* Parse a 64-hex-char key and provision it; returns 0 on success, -1 on a
 * malformed string (node state unchanged). Shared by the dev/bench seeding
 * paths so key parsing exists once. */
int network_key_set_from_hex(const char* hex);

/* Reverts to unprovisioned IN MEMORY (does NOT erase persisted storage).
 * After this, network_key_get() fails closed until a key is set, generated,
 * or loaded again. */
void network_key_clear(void);

/* Copies the provisioned key into key_out and returns 0 IFF provisioned.
 * When unprovisioned, returns nonzero and writes NOTHING to key_out
 * (fail-closed: there is no fallback key material). */
int network_key_get(uint8_t key_out[32]);

/* 1 if a real key is currently provisioned (set, generated, or loaded since
 * boot and not since network_key_clear'd), 0 otherwise. */
int network_key_is_provisioned(void);

/*
 * Generates a fresh 32-byte network key from the entropy-gated crypto_random
 * source (the SEC-L1 fail-closed source crypto_generate_identity uses),
 * provisions it (in memory + NVS), and copies it into key_out. Returns 0 on
 * success. On entropy failure returns nonzero and provisions NOTHING
 * (fail-closed): the previous provisioning state and key_out are left
 * untouched.
 */
int network_key_generate_provision(uint8_t key_out[32]);

/*
 * Loads a persisted network key from storage and provisions it in memory.
 * Returns 0 if a key was loaded, nonzero if none is stored. Does NOT
 * re-persist (load is a read). Called at boot by mesh_load_network_key
 * (main/mesh_persist.c).
 */
int network_key_load_from_nvs(void);

/*
 * Fail-closed domain-separated control-plane MAC: HMAC(network_key,
 * label || data)[0:8]. Returns 0 and writes the MAC to out when provisioned.
 * When UNPROVISIONED, returns nonzero and writes the all-zero sentinel to out
 * (it never computes an HMAC over a fallback or zeroed key). Callers MUST
 * check the return value: on nonzero they must refuse to originate or verify
 * the message rather than trust the sentinel bytes. The
 * per-type label (e.g. "bramble-rrep-v2") domain-separates message types.
 * len is the data length only (not the label) and is bounded at RUNTIME (255
 * bytes of data, 32 bytes of label): an over-long request returns nonzero and
 * the all-zero sentinel rather than overrunning the internal scratch buffer.
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
