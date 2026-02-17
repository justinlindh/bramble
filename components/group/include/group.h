#ifndef BRAMBLE_GROUP_H
#define BRAMBLE_GROUP_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define GROUP_MAX_MEMBERS    8
#define GROUP_MAX_GROUPS     8
#define GROUP_ID_SIZE        8    /* BLAKE2s truncated */
#define GROUP_KEY_SIZE       32
#define GROUP_NAME_MAX       32
#define GROUP_EPOCH_ADVANCE_THRESHOLD 256  /* messages before epoch advance */

typedef struct {
    uint32_t addr;
    bool active;
} group_member_t;

typedef struct {
    uint8_t group_id[GROUP_ID_SIZE];
    char name[GROUP_NAME_MAX];
    uint8_t group_key[GROUP_KEY_SIZE];
    uint16_t epoch;
    uint32_t message_count;
    group_member_t members[GROUP_MAX_MEMBERS];
    int member_count;
    bool active;
    uint32_t created_at_ms;
} bramble_group_t;

typedef struct {
    bramble_group_t groups[GROUP_MAX_GROUPS];
    int count;
} group_manager_t;

/* Manager */
void group_init(group_manager_t *mgr);

/* Group lifecycle */
int group_create(group_manager_t *mgr, const char *name, uint32_t creator_addr,
                 const uint32_t *member_addrs, int num_members, uint32_t now_ms);
int group_delete(group_manager_t *mgr, const uint8_t *group_id);
bramble_group_t *group_find(group_manager_t *mgr, const uint8_t *group_id);
bramble_group_t *group_find_by_name(group_manager_t *mgr, const char *name);

/* Membership */
int group_add_member(bramble_group_t *group, uint32_t addr);
int group_remove_member(bramble_group_t *group, uint32_t addr);
bool group_is_member(const bramble_group_t *group, uint32_t addr);

/* Key management */
int group_derive_key(const char *name, const uint32_t *sorted_members, int count,
                     uint8_t *key_out, uint8_t *id_out);
int group_advance_epoch(bramble_group_t *group);

/* Message tracking */
void group_record_message(bramble_group_t *group);

/* Serialization — group invite packet */
#define GROUP_INVITE_SIZE (GROUP_ID_SIZE + GROUP_KEY_SIZE + GROUP_NAME_MAX + 2)  /* +epoch */
int group_invite_serialize(const bramble_group_t *group, uint8_t *buf, size_t buf_len);
int group_invite_deserialize(const uint8_t *buf, size_t len, uint8_t *group_id_out,
                             uint8_t *key_out, char *name_out, uint16_t *epoch_out);

#endif
