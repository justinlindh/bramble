#include "replay_window.h"
#include <string.h>

void replay_table_init(replay_table_t* t) { memset(t, 0, sizeof(*t)); }

int replay_table_is_dirty(const replay_table_t* t) { return t->dirty ? 1 : 0; }
void replay_table_mark_clean(replay_table_t* t) { t->dirty = 0; }

/* Bitwise CRC-32 (IEEE 802.3, reflected). No lookup table on purpose: this
 * runs once per NVS flush (minutes apart) and once at boot, so the 8
 * iterations per byte are free, and a 1 KiB table is not. */
static uint32_t replay_crc32(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
    }
    return crc ^ 0xFFFFFFFFu;
}

/* Find or LRU-allocate a slot for src_addr. A slot handed to a (possibly
 * new) sender always starts with seen = 0: both the never-used path and the
 * LRU-reused path must reset it, or a reused slot's leftover seen = 1 from
 * its PREVIOUS occupant would skip the fresh-slot branch for the new
 * sender's first packet and misclassify it via the general accept/dup path
 * (the same failure mode the fresh-slot check in replay_check_and_add
 * guards at init, just reachable through eviction instead).
 *
 * Returns NULL rather than evicting a slot whose sender has been active
 * within REPLAY_EVICT_MIN_IDLE_MS. See the header for why refusing the new
 * sender is the correct direction to fail. */
static replay_slot_t* slot_for(replay_table_t* t, uint32_t src_addr, uint32_t now_ms) {
    replay_slot_t* lru = &t->slots[0];
    for (int i = 0; i < REPLAY_MAX_SENDERS; i++) {
        replay_slot_t* s = &t->slots[i];
        if (s->used && s->src_addr == src_addr)
            return s;
        if (!s->used) {
            s->used = 1;
            s->src_addr = src_addr;
            s->high_water = 0;
            s->window = 0;
            s->seen = 0;
            s->last_seen_ms = now_ms;
            t->dirty = 1;
            return s;
        }
        if ((uint32_t)(now_ms - s->last_seen_ms) > (uint32_t)(now_ms - lru->last_seen_ms))
            lru = s;
    }

    /* Every slot is occupied. Only recycle one that has gone quiet; an
     * attacker spoofing source addresses must not be able to evict a live
     * sender's high-water mark on demand. */
    if ((uint32_t)(now_ms - lru->last_seen_ms) < REPLAY_EVICT_MIN_IDLE_MS) {
        t->evict_denied++;
        return NULL;
    }

    t->evictions++;
    lru->src_addr = src_addr;
    lru->high_water = 0;
    lru->window = 0;
    lru->seen = 0;
    lru->last_seen_ms = now_ms;
    t->dirty = 1;
    return lru;
}

int replay_check_and_add(replay_table_t* t, uint32_t src_addr, uint64_t counter, uint32_t now_ms) {
    replay_slot_t* s = slot_for(t, src_addr, now_ms);
    if (!s)
        return REPLAY_REJECT_NO_SLOT; /* fail closed: never accept unprotected */
    s->last_seen_ms = now_ms;

    /* A fresh slot is "no packet seen yet", not "high_water
     * happens to be 0". The nonce counter issues 0 as the very
     * first value on a node's first-ever boot, so high_water == 0 is a
     * legitimate, already-seen counter value, not just an unset sentinel.
     * Conflating the two lets a replay of that real counter-0 packet re-hit
     * the "fresh slot" branch and be accepted again, forever. */
    if (!s->seen) {
        s->seen = 1;
        s->high_water = counter;
        t->dirty = 1;
        return REPLAY_ACCEPT;
    }

    if (counter > s->high_water) {
        uint64_t shift = counter - s->high_water;
        /* At shift == 64 the previous high_water sits at window bit 63 (the
         * last bit the 64-bit window can represent) and must survive the
         * shift, not be wiped to 0. A naive `shift >= 64 -> window = 0`
         * loses that bit, so replaying that previous high_water after an
         * exact 64-counter jump would wrongly be accepted. `window << 64` is
         * also undefined behavior in C, so shift must never reach the shift
         * operator at 64 or above. */
        if (shift > 64) {
            s->window = 0;
        } else if (shift == 64) {
            s->window = (1ull << 63); /* only the previous high_water still tracked */
        } else {
            s->window = (s->window << shift) | (1ull << (shift - 1));
        }
        s->high_water = counter;
        t->dirty = 1;
        return REPLAY_ACCEPT;
    }

    uint64_t delta = s->high_water - counter;
    if (delta == 0)
        return REPLAY_REJECT_DUP;
    if (delta > 64)
        return REPLAY_BELOW_WINDOW;
    uint64_t mask = 1ull << (delta - 1);
    if (s->window & mask)
        return REPLAY_REJECT_DUP;
    s->window |= mask;
    return REPLAY_ACCEPT;
}

static void put_u32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_u32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put_u64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static uint64_t get_u64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v |= ((uint64_t)p[i]) << (8 * i);
    return v;
}

int replay_table_serialize(const replay_table_t* t, uint8_t* buf, size_t buf_len) {
    size_t n = 0;
    for (int i = 0; i < REPLAY_MAX_SENDERS; i++) {
        if (t->slots[i].used && t->slots[i].seen)
            n++;
    }
    size_t need = 4u + n * 12u + 4u;
    if (buf_len < need)
        return -1;

    buf[0] = (uint8_t)REPLAY_TABLE_BLOB_VERSION;
    buf[1] = (uint8_t)n;
    buf[2] = 0;
    buf[3] = 0;
    size_t off = 4;
    for (int i = 0; i < REPLAY_MAX_SENDERS; i++) {
        const replay_slot_t* s = &t->slots[i];
        if (!s->used || !s->seen)
            continue;
        put_u32(buf + off, s->src_addr);
        put_u64(buf + off + 4, s->high_water);
        off += 12;
    }
    put_u32(buf + off, replay_crc32(buf, off));
    return (int)(off + 4);
}

int replay_table_deserialize(replay_table_t* t, const uint8_t* buf, size_t len, uint32_t now_ms) {
    replay_table_init(t);
    if (len < 8u)
        return -1;
    if (buf[0] != (uint8_t)REPLAY_TABLE_BLOB_VERSION)
        return -1;
    if (buf[2] != 0 || buf[3] != 0)
        return -1;
    size_t n = buf[1];
    if (n > REPLAY_MAX_SENDERS)
        return -1;
    size_t need = 4u + n * 12u + 4u;
    if (len != need)
        return -1;
    size_t body = 4u + n * 12u;
    if (get_u32(buf + body) != replay_crc32(buf, body))
        return -1;

    size_t off = 4;
    for (size_t i = 0; i < n; i++) {
        replay_slot_t* s = &t->slots[i];
        s->used = 1;
        s->seen = 1;
        s->src_addr = get_u32(buf + off);
        s->high_water = get_u64(buf + off + 4);
        /* Fail closed: the bitmap is not persisted, so treat the whole
         * 64-wide band below high_water as already delivered. */
        s->window = ~0ull;
        s->last_seen_ms = now_ms;
        off += 12;
    }
    /* Freshly loaded state matches what NVS already holds. */
    t->dirty = 0;
    return 0;
}
