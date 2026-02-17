/*
 * test_group.c — Unit tests for the group DM component
 */

#include "group.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) static void name(void)
#define RUN(name) do { \
    printf("  %-40s", #name); \
    name(); \
    printf(" PASS\n"); \
    tests_passed++; \
} while(0)

#define ASSERT(cond) do { if (!(cond)) { \
    printf(" FAIL (%s:%d: %s)\n", __FILE__, __LINE__, #cond); \
    tests_failed++; return; } } while(0)

TEST(test_group_create) {
    group_manager_t mgr;
    group_init(&mgr);

    uint32_t members[] = {0x02, 0x03};
    int rc = group_create(&mgr, "test-group", 0x01, members, 2, 1000);
    ASSERT(rc == 0);
    ASSERT(mgr.count == 1);

    bramble_group_t *g = group_find_by_name(&mgr, "test-group");
    ASSERT(g != NULL);
    ASSERT(g->active);
    ASSERT(g->member_count == 3);
    ASSERT(g->epoch == 0);
    ASSERT(g->created_at_ms == 1000);

    /* Verify ID and key are non-zero */
    int id_nonzero = 0, key_nonzero = 0;
    for (int i = 0; i < GROUP_ID_SIZE; i++) if (g->group_id[i]) id_nonzero = 1;
    for (int i = 0; i < GROUP_KEY_SIZE; i++) if (g->group_key[i]) key_nonzero = 1;
    ASSERT(id_nonzero);
    ASSERT(key_nonzero);
}

TEST(test_group_membership) {
    group_manager_t mgr;
    group_init(&mgr);

    uint32_t members[] = {0x02};
    group_create(&mgr, "mem-test", 0x01, members, 1, 0);
    bramble_group_t *g = group_find_by_name(&mgr, "mem-test");
    ASSERT(g != NULL);

    ASSERT(group_is_member(g, 0x01));
    ASSERT(group_is_member(g, 0x02));
    ASSERT(!group_is_member(g, 0x03));

    /* Add member */
    ASSERT(group_add_member(g, 0x03) == 0);
    ASSERT(group_is_member(g, 0x03));

    /* Remove member */
    ASSERT(group_remove_member(g, 0x03) == 0);
    ASSERT(!group_is_member(g, 0x03));

    /* Can't remove non-member */
    ASSERT(group_remove_member(g, 0x99) == -1);

    /* Can't add duplicate */
    ASSERT(group_add_member(g, 0x01) == -1);
}

TEST(test_group_max_members) {
    group_manager_t mgr;
    group_init(&mgr);

    /* Create with 7 members + creator = 8 (max) */
    uint32_t members[7];
    for (int i = 0; i < 7; i++) members[i] = (uint32_t)(i + 2);
    int rc = group_create(&mgr, "full", 0x01, members, 7, 0);
    ASSERT(rc == 0);

    bramble_group_t *g = group_find_by_name(&mgr, "full");
    ASSERT(g != NULL);
    ASSERT(g->member_count == 8);

    /* 9th member should fail */
    ASSERT(group_add_member(g, 0xFF) == -1);
}

TEST(test_group_max_groups) {
    group_manager_t mgr;
    group_init(&mgr);

    char name[32];
    uint32_t mem[] = {0x02};
    for (int i = 0; i < GROUP_MAX_GROUPS; i++) {
        snprintf(name, sizeof(name), "group-%d", i);
        ASSERT(group_create(&mgr, name, (uint32_t)(i + 1), mem, 1, 0) == 0);
    }
    ASSERT(mgr.count == GROUP_MAX_GROUPS);

    /* 9th should fail */
    ASSERT(group_create(&mgr, "overflow", 0xFF, mem, 1, 0) == -1);
}

TEST(test_group_find) {
    group_manager_t mgr;
    group_init(&mgr);

    uint32_t mem[] = {0x02};
    group_create(&mgr, "findme", 0x01, mem, 1, 0);

    bramble_group_t *g = group_find_by_name(&mgr, "findme");
    ASSERT(g != NULL);

    /* Find by ID */
    bramble_group_t *g2 = group_find(&mgr, g->group_id);
    ASSERT(g2 == g);

    /* Not found */
    ASSERT(group_find_by_name(&mgr, "nope") == NULL);
    uint8_t fake_id[GROUP_ID_SIZE] = {0};
    ASSERT(group_find(&mgr, fake_id) == NULL);
}

TEST(test_group_epoch_advance) {
    group_manager_t mgr;
    group_init(&mgr);

    uint32_t mem[] = {0x02};
    group_create(&mgr, "epoch", 0x01, mem, 1, 0);
    bramble_group_t *g = group_find_by_name(&mgr, "epoch");
    ASSERT(g != NULL);

    uint8_t orig_key[GROUP_KEY_SIZE];
    memcpy(orig_key, g->group_key, GROUP_KEY_SIZE);

    ASSERT(g->epoch == 0);
    for (int i = 0; i < GROUP_EPOCH_ADVANCE_THRESHOLD; i++) {
        group_record_message(g);
    }
    ASSERT(g->epoch == 1);
    ASSERT(g->message_count == 0);

    /* Key should have changed */
    ASSERT(memcmp(orig_key, g->group_key, GROUP_KEY_SIZE) != 0);
}

TEST(test_group_invite_roundtrip) {
    group_manager_t mgr;
    group_init(&mgr);

    uint32_t mem[] = {0x02, 0x03};
    group_create(&mgr, "invite-test", 0x01, mem, 2, 5000);
    bramble_group_t *g = group_find_by_name(&mgr, "invite-test");
    ASSERT(g != NULL);

    uint8_t buf[GROUP_INVITE_SIZE];
    ASSERT(group_invite_serialize(g, buf, sizeof(buf)) == 0);

    uint8_t id_out[GROUP_ID_SIZE], key_out[GROUP_KEY_SIZE];
    char name_out[GROUP_NAME_MAX];
    uint16_t epoch_out;
    ASSERT(group_invite_deserialize(buf, sizeof(buf), id_out, key_out, name_out, &epoch_out) == 0);

    ASSERT(memcmp(id_out, g->group_id, GROUP_ID_SIZE) == 0);
    ASSERT(memcmp(key_out, g->group_key, GROUP_KEY_SIZE) == 0);
    ASSERT(strcmp(name_out, "invite-test") == 0);
    ASSERT(epoch_out == 0);

    /* Too-small buffer */
    ASSERT(group_invite_serialize(g, buf, 5) == -1);
    ASSERT(group_invite_deserialize(buf, 5, id_out, key_out, name_out, &epoch_out) == -1);
}

TEST(test_group_delete) {
    group_manager_t mgr;
    group_init(&mgr);

    uint32_t mem[] = {0x02};
    group_create(&mgr, "deleteme", 0x01, mem, 1, 0);
    ASSERT(mgr.count == 1);

    bramble_group_t *g = group_find_by_name(&mgr, "deleteme");
    ASSERT(g != NULL);

    uint8_t id[GROUP_ID_SIZE];
    memcpy(id, g->group_id, GROUP_ID_SIZE);

    ASSERT(group_delete(&mgr, id) == 0);
    ASSERT(mgr.count == 0);
    ASSERT(group_find(&mgr, id) == NULL);

    /* Slot should be reusable */
    ASSERT(group_create(&mgr, "reuse", 0x01, mem, 1, 0) == 0);
    ASSERT(mgr.count == 1);
}

int main(void) {
    printf("test_group:\n");
    RUN(test_group_create);
    RUN(test_group_membership);
    RUN(test_group_max_members);
    RUN(test_group_max_groups);
    RUN(test_group_find);
    RUN(test_group_epoch_advance);
    RUN(test_group_invite_roundtrip);
    RUN(test_group_delete);

    printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed ? 1 : 0;
}
