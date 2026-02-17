/*
 * group.c — Group DM management for Bramble
 *
 * Key derivation uses FNV-1a for portability (no OpenSSL dependency).
 * Production deployments should replace with HKDF-SHA256 / BLAKE2s.
 */

#include "group.h"
#include <string.h>
#include <stdlib.h>

/* ---- FNV-1a helpers ---- */

static uint64_t fnv1a_init(void) {
    return 0xcbf29ce484222325ULL;
}

static uint64_t fnv1a_update(uint64_t h, const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        h ^= data[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

/* Fill buf with deterministic bytes seeded from h */
static void fnv1a_expand(uint64_t seed, uint8_t *buf, size_t len) {
    uint64_t h = seed;
    for (size_t i = 0; i < len; i++) {
        h = fnv1a_update(h, (const uint8_t *)&i, sizeof(i));
        buf[i] = (uint8_t)(h & 0xff);
    }
}

/* ---- comparator for qsort ---- */

static int cmp_u32(const void *a, const void *b) {
    uint32_t va = *(const uint32_t *)a;
    uint32_t vb = *(const uint32_t *)b;
    return (va > vb) - (va < vb);
}

/* ---- API ---- */

void group_init(group_manager_t *mgr) {
    memset(mgr, 0, sizeof(*mgr));
}

int group_derive_key(const char *name, const uint32_t *sorted_members, int count,
                     uint8_t *key_out, uint8_t *id_out) {
    if (!name || !sorted_members || count <= 0) return -1;

    uint64_t h = fnv1a_init();
    h = fnv1a_update(h, (const uint8_t *)name, strlen(name));
    h = fnv1a_update(h, (const uint8_t *)sorted_members, (size_t)count * sizeof(uint32_t));

    if (id_out) {
        /* First 8 bytes from one expansion */
        fnv1a_expand(h ^ 0x4944ULL, id_out, GROUP_ID_SIZE);  /* "ID" */
    }
    if (key_out) {
        /* 32 bytes from different expansion */
        fnv1a_expand(h ^ 0x4b4559ULL, key_out, GROUP_KEY_SIZE);  /* "KEY" */
    }
    return 0;
}

int group_create(group_manager_t *mgr, const char *name, uint32_t creator_addr,
                 const uint32_t *member_addrs, int num_members, uint32_t now_ms) {
    if (!mgr || !name || num_members < 0) return -1;
    if (mgr->count >= GROUP_MAX_GROUPS) return -1;
    if (num_members + 1 > GROUP_MAX_MEMBERS) return -1;  /* +creator */

    /* Build sorted member list (creator + provided members) */
    uint32_t all[GROUP_MAX_MEMBERS];
    int total = 0;
    all[total++] = creator_addr;
    for (int i = 0; i < num_members; i++) {
        /* skip duplicates of creator */
        if (member_addrs[i] == creator_addr) continue;
        if (total >= GROUP_MAX_MEMBERS) return -1;
        all[total++] = member_addrs[i];
    }
    qsort(all, (size_t)total, sizeof(uint32_t), cmp_u32);

    /* Find free slot */
    bramble_group_t *g = NULL;
    for (int i = 0; i < GROUP_MAX_GROUPS; i++) {
        if (!mgr->groups[i].active) {
            g = &mgr->groups[i];
            break;
        }
    }
    if (!g) return -1;

    memset(g, 0, sizeof(*g));
    strncpy(g->name, name, GROUP_NAME_MAX - 1);
    g->name[GROUP_NAME_MAX - 1] = '\0';

    group_derive_key(name, all, total, g->group_key, g->group_id);

    for (int i = 0; i < total; i++) {
        g->members[i].addr = all[i];
        g->members[i].active = true;
    }
    g->member_count = total;
    g->epoch = 0;
    g->message_count = 0;
    g->active = true;
    g->created_at_ms = now_ms;
    mgr->count++;

    return 0;
}

int group_delete(group_manager_t *mgr, const uint8_t *group_id) {
    if (!mgr || !group_id) return -1;
    bramble_group_t *g = group_find(mgr, group_id);
    if (!g) return -1;
    g->active = false;
    mgr->count--;
    return 0;
}

bramble_group_t *group_find(group_manager_t *mgr, const uint8_t *group_id) {
    if (!mgr || !group_id) return NULL;
    for (int i = 0; i < GROUP_MAX_GROUPS; i++) {
        if (mgr->groups[i].active &&
            memcmp(mgr->groups[i].group_id, group_id, GROUP_ID_SIZE) == 0) {
            return &mgr->groups[i];
        }
    }
    return NULL;
}

bramble_group_t *group_find_by_name(group_manager_t *mgr, const char *name) {
    if (!mgr || !name) return NULL;
    for (int i = 0; i < GROUP_MAX_GROUPS; i++) {
        if (mgr->groups[i].active && strcmp(mgr->groups[i].name, name) == 0) {
            return &mgr->groups[i];
        }
    }
    return NULL;
}

int group_add_member(bramble_group_t *group, uint32_t addr) {
    if (!group) return -1;
    if (group_is_member(group, addr)) return -1;
    if (group->member_count >= GROUP_MAX_MEMBERS) return -1;
    group->members[group->member_count].addr = addr;
    group->members[group->member_count].active = true;
    group->member_count++;
    return 0;
}

int group_remove_member(bramble_group_t *group, uint32_t addr) {
    if (!group) return -1;
    for (int i = 0; i < group->member_count; i++) {
        if (group->members[i].active && group->members[i].addr == addr) {
            group->members[i].active = false;
            return 0;
        }
    }
    return -1;
}

bool group_is_member(const bramble_group_t *group, uint32_t addr) {
    if (!group) return false;
    for (int i = 0; i < group->member_count; i++) {
        if (group->members[i].active && group->members[i].addr == addr)
            return true;
    }
    return false;
}

int group_advance_epoch(bramble_group_t *group) {
    if (!group) return -1;
    group->epoch++;

    /* Re-derive key: mix epoch into existing key via FNV-1a */
    uint64_t h = fnv1a_init();
    h = fnv1a_update(h, group->group_key, GROUP_KEY_SIZE);
    h = fnv1a_update(h, (const uint8_t *)&group->epoch, sizeof(group->epoch));
    fnv1a_expand(h, group->group_key, GROUP_KEY_SIZE);

    return 0;
}

void group_record_message(bramble_group_t *group) {
    if (!group) return;
    group->message_count++;
    if (group->message_count >= GROUP_EPOCH_ADVANCE_THRESHOLD) {
        group_advance_epoch(group);
        group->message_count = 0;
    }
}

int group_invite_serialize(const bramble_group_t *group, uint8_t *buf, size_t buf_len) {
    if (!group || !buf || buf_len < GROUP_INVITE_SIZE) return -1;

    size_t off = 0;
    memcpy(buf + off, group->group_id, GROUP_ID_SIZE); off += GROUP_ID_SIZE;
    memcpy(buf + off, group->group_key, GROUP_KEY_SIZE); off += GROUP_KEY_SIZE;
    memcpy(buf + off, group->name, GROUP_NAME_MAX); off += GROUP_NAME_MAX;
    buf[off++] = (uint8_t)(group->epoch >> 8);
    buf[off++] = (uint8_t)(group->epoch & 0xff);

    return 0;
}

int group_invite_deserialize(const uint8_t *buf, size_t len, uint8_t *group_id_out,
                             uint8_t *key_out, char *name_out, uint16_t *epoch_out) {
    if (!buf || len < GROUP_INVITE_SIZE) return -1;

    size_t off = 0;
    if (group_id_out) memcpy(group_id_out, buf + off, GROUP_ID_SIZE);
    off += GROUP_ID_SIZE;
    if (key_out) memcpy(key_out, buf + off, GROUP_KEY_SIZE);
    off += GROUP_KEY_SIZE;
    if (name_out) {
        memcpy(name_out, buf + off, GROUP_NAME_MAX);
        name_out[GROUP_NAME_MAX - 1] = '\0';
    }
    off += GROUP_NAME_MAX;
    if (epoch_out) *epoch_out = (uint16_t)((buf[off] << 8) | buf[off + 1]);

    return 0;
}
