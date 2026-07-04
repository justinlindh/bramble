#include "rreq_pseudonym.h"
#include <string.h>
#include "crypto.h"

uint32_t rreq_pseudonym_generate(const uint8_t* private_key, uint32_t address, uint32_t query_id) {
    uint8_t input[8];
    memcpy(input, &address, 4);
    memcpy(input + 4, &query_id, 4);

    uint8_t hmac_out[32];
    crypto_hmac_sha256(private_key, BRAMBLE_KEY_SIZE, input, sizeof(input), hmac_out);

    /* Truncate to 4 bytes (same size as Bramble address) */
    uint32_t pseudonym;
    memcpy(&pseudonym, hmac_out, 4);
    return pseudonym;
}
