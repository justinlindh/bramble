#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#ifdef UNIT_TEST
#include "esp_stubs.h"
#else
#include "esp_err.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

bool mesh_should_emit_broadcast_delivery_receipt(uint32_t dest_addr);

esp_err_t mesh_build_broadcast_delivery_receipt_packet(uint32_t local_addr,
                                                       uint32_t receipt_packet_id,
                                                       uint32_t original_src_addr,
                                                       uint32_t original_packet_id,
                                                       uint8_t *buf,
                                                       size_t buf_len,
                                                       size_t *out_len);

#ifdef __cplusplus
}
#endif
