/*
 * Roll-call firmware entry points for the host test targets that compile
 * main/rpc_methods.c. The real main/mesh_rollcall.c needs a radio, an
 * identity store and a heap, none of which exist on the host, so the entry
 * points live here instead.
 *
 * DRIVEABLE, not inert: the roll-call RPC handlers are themselves under test
 * in test_rpc_rollcall.c, so a test picks the start outcome and fills
 * g_rollcall_ledger through the REAL components/rollcall API. What the
 * handler serializes is therefore what the firmware's own ledger would have
 * handed it.
 *
 * One shared file rather than a copy in each of the two rpc_methods stub
 * sets (rpc_methods_link_stubs.c and rpc_methods_test_stubs.c), which are
 * never linked together but would otherwise both need these symbols and
 * could drift apart.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The real prototypes, so a signature change in the firmware breaks this
 * file at compile time rather than at link time. */
#include "mesh_rollcall.h"

int g_rollcall_start_result = MESH_ROLLCALL_OK;
uint32_t g_rollcall_next_id = 0x0000BEEF;
uint32_t g_rollcall_pending_dropped = 0;
uint32_t g_rollcall_answer_limited = 0;
bool g_rollcall_ledger_present = false;
rollcall_ledger_t g_rollcall_ledger;

int mesh_rollcall_start(const char* text, size_t text_len, uint32_t* rollcall_id_out) {
    (void)text;
    (void)text_len;
    if (g_rollcall_start_result != MESH_ROLLCALL_OK)
        return g_rollcall_start_result;
    if (rollcall_id_out != NULL)
        *rollcall_id_out = g_rollcall_next_id;
    return MESH_ROLLCALL_OK;
}

const rollcall_ledger_t* mesh_rollcall_ledger(void) {
    return g_rollcall_ledger_present ? &g_rollcall_ledger : NULL;
}

uint32_t mesh_rollcall_pending_dropped(void) { return g_rollcall_pending_dropped; }

uint32_t mesh_rollcall_answer_limited(void) { return g_rollcall_answer_limited; }

uint32_t g_rollcall_retry_after_ms = 0;
uint32_t mesh_rollcall_retry_after_ms(void) { return g_rollcall_retry_after_ms; }

bool mesh_rollcall_handle_announce(uint32_t src_addr, int channel_idx, const uint8_t* data,
                                   size_t data_len) {
    (void)src_addr;
    (void)channel_idx;
    (void)data;
    (void)data_len;
    return false;
}

void mesh_rollcall_handle_response(uint32_t src_addr, const uint8_t* data, size_t data_len) {
    (void)src_addr;
    (void)data;
    (void)data_len;
}

void mesh_rollcall_note_receipt(uint32_t responder_addr, uint32_t orig_packet_id, uint8_t hop_count,
                                const uint32_t* relay_path) {
    (void)responder_addr;
    (void)orig_packet_id;
    (void)hop_count;
    (void)relay_path;
}

void mesh_rollcall_tick(uint32_t t) { (void)t; }
