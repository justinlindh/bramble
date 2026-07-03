#ifndef BRAMBLE_DM_SESSION_H
#define BRAMBLE_DM_SESSION_H
#include <stdint.h>
#include "crypto.h"

int dm_derive_session_key(const uint8_t my_id_priv[32], const uint8_t my_eph_priv[32],
                          const uint8_t peer_id_pub[32], const uint8_t peer_eph_pub[32],
                          uint32_t addr_a, uint32_t addr_b, uint16_t ke_epoch,
                          uint8_t session_key_out[32]);
int dm_derive_sas(const uint8_t session_ikm[128], char sas_out[8]);
#endif
