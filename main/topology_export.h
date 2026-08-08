#pragma once

/*
 * topology_export: the bramble.exportTopology document builder.
 *
 * One node's observed mesh state as a single JSON document: who it is, the
 * neighbours it hears and the link quality it hears them at, its routing
 * table, and the PHY and regulatory parameters that decide time-on-air. The
 * simulator's digital-twin importer (simulator/gosim, docs/digital-twin.md)
 * reconstructs a deployment from a collection of these.
 *
 * It lives here, apart from main/rpc_methods.c, because two very different
 * callers have to produce byte-identical documents:
 *
 *   - firmware, where rpc_methods.c reads the tables out of mesh_get_state /
 *     mesh_get_routes and answers an RPC with the result;
 *   - the simulator, which compiles this file into gosim (simulator/gosim/all.c)
 *     and hands it a simulated node's own neighbour_table_t / routing_table_t.
 *
 * That is what makes the twin's round-trip test meaningful: the export the
 * simulator re-imports is written by the firmware's export code, not by a
 * second implementation that could agree with the schema and disagree with the
 * device.
 *
 * Nothing here measures anything. Every field is state the caller already
 * holds, serialized at the moment of the call.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "routing.h"

/*
 * Schema version of the document. The importer refuses a version it does not
 * know rather than guessing at fields, so this number moves whenever the
 * document's meaning changes. Mirrored by twinSchemaSupported in
 * simulator/gosim/twin_export.go and by the schema in api/openapi.yaml.
 */
#define TOPOLOGY_EXPORT_SCHEMA 1

/* Who the exporting node is. Every string may be NULL, which omits the field;
 * that is how a node with no configured name exports (the importer reads an
 * absent name as unnamed, and never has to know a placeholder). */
typedef struct {
    uint32_t address;
    const char* name;
    const char* firmware_version;
    const char* protocol_version;
    const char* hardware;
    uint64_t uptime_s;
} topology_export_identity_t;

/*
 * The exporting node's runtime PHY plus the frequency plan it runs under.
 * sf/bw_hz/coding_rate are what bramble_calculate_airtime_us prices a frame
 * at, so they are what let the twin charge the same time-on-air; the plan's
 * duty-cycle ceiling bounds the airtime the deployment may spend at all.
 * region and regulatory may be NULL, which omits them.
 */
typedef struct {
    float frequency_mhz;
    uint8_t sf;
    uint32_t bw_hz;
    uint8_t coding_rate;
    int8_t tx_power_dbm;
    const char* region;
    const char* regulatory;
    uint8_t max_duty_cycle_pct;
    bool duty_cycle_enforced;
} topology_export_phy_t;

/*
 * Serialize a neighbour table into the (already created) array `arr`.
 * now_ms is the caller's millisecond clock, used to turn each entry's
 * last_heard into an age: a node's absolute clock means nothing to a reader,
 * elapsed time means the same to everyone. Entries with address 0 are skipped,
 * so an unused table slot never becomes a link to nowhere.
 *
 * This is the one writer of the neighbour wire shape: bramble.getNeighbors
 * emits its array through it too, so a client and the twin importer cannot be
 * handed two spellings of the same observation.
 */
void topology_export_neighbors(cJSON* arr, const neighbor_table_t* table, uint32_t now_ms);

/* The route counterpart of topology_export_neighbors, shared with
 * bramble.getRoutes. */
void topology_export_routes(cJSON* arr, const routing_table_t* table);

/*
 * Write the whole export document into `result`, an already created object.
 * neighbors and routes may be NULL, which emits an empty array for that
 * section rather than omitting it: the importer always finds both keys.
 */
void topology_export_document(cJSON* result, const topology_export_identity_t* identity,
                              const topology_export_phy_t* phy, const neighbor_table_t* neighbors,
                              const routing_table_t* routes, uint32_t now_ms);
