/*
 * group.c — Group DM management for Bramble
 *
 * Key derivation uses HKDF-SHA256 (via mbedTLS through crypto component).
 * Invite packets encrypt the group key under X25519 + AES-256-GCM.
 */

#include "group.h"
#include <string.h>
#include <stdlib.h>

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

    /*
     * HKDF-SHA256:
     *   IKM  = sorted member addresses (concatenated raw bytes)
     *   salt = "bramble-group-v1"
     *   info = group name
     *
     * Derive 40 bytes: first 32 = group key, last 8 = group ID.
     */
    static const char *salt = "bramble-group-v1";
    uint8_t okm[GROUP_KEY_SIZE + GROUP_ID_SIZE]; /* 40 bytes */

    int ret = crypto_hkdf_sha256(
        (const uint8_t *)salt, strlen(salt),
        (const uint8_t *)sorted_members, (size_t)count * sizeof(uint32_t),
        (const uint8_t *)name, strlen(name),
        okm, sizeof(okm));
    if (ret != 0) return -1;

    if (key_out) memcpy(key_out, okm, GROUP_KEY_SIZE);
    if (id_out)  memcpy(id_out, okm + GROUP_KEY_SIZE, GROUP_ID_SIZE);

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

    /* Re-derive key via HKDF-SHA256: old key as IKM, epoch in info */
    static const char *salt = "bramble-group-epoch";
    uint8_t info[2];
    info[0] = (uint8_t)(group->epoch >> 8);
    info[1] = (uint8_t)(group->epoch & 0xFF);

    uint8_t new_key[GROUP_KEY_SIZE];
    if (crypto_hkdf_sha256((const uint8_t *)salt, strlen(salt),
                           group->group_key, GROUP_KEY_SIZE,
                           info, 2,
                           new_key, GROUP_KEY_SIZE) != 0) {
        return -1;
    }
    memcpy(group->group_key, new_key, GROUP_KEY_SIZE);

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

int group_invite_serialize(const bramble_group_t *group,
                           const uint8_t *sender_private_key,
                           const uint8_t *recipient_public_key,
                           uint8_t *buf, size_t buf_len) {
    if (!group || !sender_private_key || !recipient_public_key ||
        !buf || buf_len < GROUP_INVITE_SIZE) return -1;

    /*
     * Generate an ephemeral X25519 keypair, perform DH with recipient's
     * public key, derive an encryption key via HKDF, and encrypt the
     * group_key with AES-256-GCM.  AAD = group_id for binding.
     *
     * Layout:
     *   [group_id: 8] [ephemeral_pub: 32] [encrypted_key: 32] [tag: 16]
     *   [name: 32] [epoch: 2]
     */

    /* Generate ephemeral keypair */
    bramble_identity_t eph;
    if (crypto_generate_identity(&eph) != 0) return -1;

    /* X25519 DH: ephemeral private × recipient public */
    uint8_t shared_secret[BRAMBLE_KEY_SIZE];
    if (crypto_x25519_dh(eph.private_key, recipient_public_key, shared_secret) != 0)
        return -1;

    /* Derive encryption key from shared secret */
    static const char *hkdf_salt = "bramble-group-invite-v1";
    uint8_t enc_key[BRAMBLE_KEY_SIZE];
    if (crypto_hkdf_sha256((const uint8_t *)hkdf_salt, strlen(hkdf_salt),
                           shared_secret, BRAMBLE_KEY_SIZE,
                           group->group_id, GROUP_ID_SIZE,
                           enc_key, BRAMBLE_KEY_SIZE) != 0)
        return -1;

    /* Build nonce from first 12 bytes of ephemeral pubkey */
    uint8_t nonce[BRAMBLE_NONCE_SIZE];
    memcpy(nonce, eph.public_key, BRAMBLE_NONCE_SIZE);

    /* Encrypt group_key */
    uint8_t encrypted_key[GROUP_KEY_SIZE];
    uint8_t tag[BRAMBLE_TAG_SIZE];
    if (crypto_aes256gcm_encrypt(enc_key, nonce,
                                  group->group_key, GROUP_KEY_SIZE,
                                  group->group_id, GROUP_ID_SIZE,
                                  encrypted_key, tag) != 0)
        return -1;

    /* Serialize */
    size_t off = 0;
    memcpy(buf + off, group->group_id, GROUP_ID_SIZE);      off += GROUP_ID_SIZE;
    memcpy(buf + off, eph.public_key, BRAMBLE_KEY_SIZE);     off += BRAMBLE_KEY_SIZE;
    memcpy(buf + off, encrypted_key, GROUP_KEY_SIZE);        off += GROUP_KEY_SIZE;
    memcpy(buf + off, tag, BRAMBLE_TAG_SIZE);                off += BRAMBLE_TAG_SIZE;
    memcpy(buf + off, group->name, GROUP_NAME_MAX);          off += GROUP_NAME_MAX;
    buf[off++] = (uint8_t)(group->epoch >> 8);
    buf[off++] = (uint8_t)(group->epoch & 0xff);

    /* Wipe sensitive intermediates */
    memset(&eph, 0, sizeof(eph));
    memset(shared_secret, 0, sizeof(shared_secret));
    memset(enc_key, 0, sizeof(enc_key));

    return 0;
}

int group_invite_deserialize(const uint8_t *buf, size_t len,
                             const uint8_t *recipient_private_key,
                             uint8_t *group_id_out,
                             uint8_t *key_out, char *name_out, uint16_t *epoch_out) {
    if (!buf || !recipient_private_key || len < GROUP_INVITE_SIZE) return -1;

    size_t off = 0;

    /* Parse fields */
    const uint8_t *group_id      = buf + off;  off += GROUP_ID_SIZE;
    const uint8_t *ephemeral_pub = buf + off;   off += BRAMBLE_KEY_SIZE;
    const uint8_t *encrypted_key = buf + off;   off += GROUP_KEY_SIZE;
    const uint8_t *tag           = buf + off;   off += BRAMBLE_TAG_SIZE;
    const uint8_t *name          = buf + off;   off += GROUP_NAME_MAX;

    /* X25519 DH: recipient private × ephemeral public */
    uint8_t shared_secret[BRAMBLE_KEY_SIZE];
    if (crypto_x25519_dh(recipient_private_key, ephemeral_pub, shared_secret) != 0)
        return -1;

    /* Derive decryption key */
    static const char *hkdf_salt = "bramble-group-invite-v1";
    uint8_t dec_key[BRAMBLE_KEY_SIZE];
    if (crypto_hkdf_sha256((const uint8_t *)hkdf_salt, strlen(hkdf_salt),
                           shared_secret, BRAMBLE_KEY_SIZE,
                           group_id, GROUP_ID_SIZE,
                           dec_key, BRAMBLE_KEY_SIZE) != 0)
        return -1;

    /* Nonce = first 12 bytes of ephemeral pubkey */
    uint8_t nonce[BRAMBLE_NONCE_SIZE];
    memcpy(nonce, ephemeral_pub, BRAMBLE_NONCE_SIZE);

    /* Decrypt group_key */
    uint8_t decrypted_key[GROUP_KEY_SIZE];
    if (crypto_aes256gcm_decrypt(dec_key, nonce,
                                  encrypted_key, GROUP_KEY_SIZE,
                                  group_id, GROUP_ID_SIZE,
                                  tag, decrypted_key) != 0)
        return -1;

    /* Output */
    if (group_id_out) memcpy(group_id_out, group_id, GROUP_ID_SIZE);
    if (key_out)      memcpy(key_out, decrypted_key, GROUP_KEY_SIZE);
    if (name_out) {
        memcpy(name_out, name, GROUP_NAME_MAX);
        name_out[GROUP_NAME_MAX - 1] = '\0';
    }
    if (epoch_out) *epoch_out = (uint16_t)((buf[off] << 8) | buf[off + 1]);

    /* Wipe */
    memset(shared_secret, 0, sizeof(shared_secret));
    memset(dec_key, 0, sizeof(dec_key));

    return 0;
}
