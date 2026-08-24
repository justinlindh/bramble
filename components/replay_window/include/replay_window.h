#ifndef BRAMBLE_REPLAY_WINDOW_H
#define BRAMBLE_REPLAY_WINDOW_H
#include <stdint.h>
#include <stddef.h>

#define REPLAY_MAX_SENDERS 64
#define REPLAY_ACCEPT 0
#define REPLAY_REJECT_DUP 1
#define REPLAY_BELOW_WINDOW 2
/* No slot could be allocated for this sender because every slot is held by a
 * sender that has been active too recently to evict. Callers MUST treat this
 * as a drop, never as an accept. */
#define REPLAY_REJECT_NO_SLOT 3

/*
 * The table is bounded and senders are not, so eviction is unavoidable. What
 * is NOT acceptable is eviction that silently reopens a replay window:
 * src_addr is spoofable, so an attacker who can force the eviction of a
 * target's slot resets that target's high_water to 0 and can then replay
 * anything ever captured from them.
 *
 * So a slot is un-evictable while its sender is still plausibly active. A
 * slot is only an eviction candidate once it has been idle for at
 * least REPLAY_EVICT_MIN_IDLE_MS; if no slot qualifies, the table refuses to
 * allocate and the NEW sender's packet is dropped (REPLAY_REJECT_NO_SLOT).
 * That is the fail-closed direction: flooding spoofed source addresses
 * costs the attacker their own delivery instead of the victim's protection.
 *
 * The trade-off is honest and deliberate: on a mesh with more than
 * REPLAY_MAX_SENDERS concurrently-active senders, a genuinely new sender can
 * be refused until an existing slot goes idle. Dropping a new sender's
 * traffic is recoverable; silently un-protecting an established one is not.
 */
#define REPLAY_EVICT_MIN_IDLE_MS (15u * 60u * 1000u)

/* Persistence blob. Layout, all little-endian:
 *   [0]      format version
 *   [1]      entry count N (<= REPLAY_MAX_SENDERS)
 *   [2..3]   reserved, must be zero
 *   [4..]    N records of { uint32 src_addr, uint64 high_water }
 *   [tail]   uint32 CRC-32 (IEEE) over every preceding byte
 * The CRC exists so a torn or corrupt blob is DETECTED rather than silently
 * loaded as a table full of bogus high-water marks. */
#define REPLAY_TABLE_BLOB_VERSION 1u
#define REPLAY_TABLE_BLOB_MAX (4u + (REPLAY_MAX_SENDERS * 12u) + 4u)

typedef struct {
    uint32_t src_addr;
    uint64_t high_water;
    uint64_t window; /* bitmap of the 64 positions below high_water */
    uint32_t last_seen_ms;
    uint8_t used;
    uint8_t seen; /* has this slot's sender ever had a packet accepted?
                   * Distinct from high_water==0, which is a legitimate
                   * counter value (the nonce counter's first-boot
                   * value), not just "nothing seen yet". */
} replay_slot_t;

typedef struct {
    replay_slot_t slots[REPLAY_MAX_SENDERS];
    /* Eviction observability, so recycling is never silent. `evictions`
     * counts slots actually recycled, `evict_denied` counts allocations
     * refused because every slot was too recently active. A climbing
     * evict_denied is the signature of a spoofed-source flood. Both are
     * RAM-only and not serialized. */
    uint32_t evictions;
    uint32_t evict_denied;
    /* Set whenever a persisted field (src_addr / high_water membership)
     * changes, cleared by replay_table_mark_clean. Lets the NVS flush skip
     * writing an unchanged table, which is what keeps flash wear bounded. */
    uint8_t dirty;
} replay_table_t;

void replay_table_init(replay_table_t* t);
int replay_check_and_add(replay_table_t* t, uint32_t src_addr, uint64_t counter, uint32_t now_ms);

/* True if the persisted view of the table is stale. */
int replay_table_is_dirty(const replay_table_t* t);
void replay_table_mark_clean(replay_table_t* t);

/*
 * Pure (NVS-free, host-testable) serialize/deserialize of the per-sender
 * high-water marks, so the replay window survives a reboot.
 *
 * serialize returns the number of bytes written, or -1 if buf_len is too
 * small. Only slots that have actually seen a packet are written.
 *
 * deserialize returns 0 on success and -1 on ANY malformed input (bad
 * version, bad length, bad count, CRC mismatch). On failure the table is
 * left initialized and empty, never half-populated.
 *
 * On success each restored slot is reconstructed with its persisted
 * high_water, seen = 1, and window = all-ones: every position in the 64-wide
 * band below high_water is marked already-seen. That is deliberately
 * fail-closed. The bitmap itself is not persisted, so the honest
 * reconstruction of "I do not know which of those 64 were delivered" is to
 * assume all of them were. The cost is that a genuinely in-flight,
 * out-of-order packet from just before the reboot is dropped at tier 1; for
 * CHAT it can still arrive through the tier-2 deferred path, which validates
 * an authenticated sent_at. The benefit is that a captured batch cannot be
 * replayed into the band.
 *
 * last_seen_ms is set to now_ms on every restored slot so that restored
 * entries are not instantly the most attractive LRU eviction victims.
 */
int replay_table_serialize(const replay_table_t* t, uint8_t* buf, size_t buf_len);
int replay_table_deserialize(replay_table_t* t, const uint8_t* buf, size_t len, uint32_t now_ms);
#endif
