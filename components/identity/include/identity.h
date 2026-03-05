#ifndef BRAMBLE_IDENTITY_H
#define BRAMBLE_IDENTITY_H

#include "crypto.h"

int identity_load(bramble_identity_t* id);
int identity_save(const bramble_identity_t* id);
int identity_generate_and_save(bramble_identity_t* id);
bool identity_check_collision(const bramble_identity_t* my_id, uint32_t beacon_src_addr,
                              uint32_t beacon_pubkey_hash);

#endif
