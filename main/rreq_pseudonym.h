#ifndef RREQ_PSEUDONYM_H
#define RREQ_PSEUDONYM_H

#include <stdint.h>

/* Derive an unlinkable 4-byte RREQ pseudonym from the node private key,
 * address, and per-query id. Pure; identical to the pre-extraction
 * mesh_task.c static of the same behavior. */
uint32_t rreq_pseudonym_generate(const uint8_t *private_key, uint32_t address, uint32_t query_id);

#endif /* RREQ_PSEUDONYM_H */
