/*
 * all.c: Single compilation unit that includes all C sources for cgo.
 * cgo compiles this as one .o file. bridge.c is compiled separately by cgo.
 */

/* ESP stubs first */
#include "../../test/stubs/esp_stubs.h"

/* No-op NVS stubs for components that persist settings (location.c) */
#define NVS_STUBS_ENABLE 1
#include "../../test/stubs/esp_stubs.c"

/* Frequency plan: a pure static table with no ESP-IDF dependencies, included
 * ahead of the simulator modules because sim_radio.c reads the plan's
 * default_sf/default_bw_hz for its PHY defaults (the same values
 * mesh_init_radio_config programs into a real node's radio). */
#include "../../components/freq_plan/freq_plan.c"

/* Simulator modules */
#include "../engine/sim_event.c"
#include "../engine/sim_random.c"
#include "../engine/sim_emitter.c"
#include "../engine/sim_node.c"
#include "../engine/sim_radio.c"
#include "../engine/sim_scenario.c"
#include "../engine/sim_metrics.c"
#include "../engine/sim_anomaly.c"
#include "../engine/cJSON.c"

/* Bramble components */
#include "../../components/network_key/network_key.c"
#include "../../components/routing/routing.c"
#include "../../components/routing/discovery.c"
#include "../../components/routing/forwarding.c"
#include "../../components/routing/channel_flood.c"
#include "../../components/packet/packet.c"
#include "../../components/reliability/reliability.c"
#include "../../components/dedup/dedup.c"
#include "../../components/airtime/airtime_budget.c"
#include "../../components/fragment/fragment.c"
#include "../../components/crypto/crypto_host.c"
#include "../../components/security/security.c"
/* routing_auth.c + identity_store.c live in identity_unit.c, and the attested
 * roll-call core in rollcall_unit.c: both are separate compilation units
 * because their static helpers (routing_auth.c's ct_eq, rollcall.c's
 * put_be32/get_be32) collide with identically named ones here. */

/* New components (Phase 6) */
#include "../../components/mailbox/mailbox.c"
#include "../../components/location/location.c"
#include "../../components/channel/channel_key.c"
#include "../../components/channel/public_channel.c"

/* Firmware policy modules: pure, no ESP-IDF deps, host-tested directly
 * (test/test_beacon_policy_calc.c). Lives under main/ but is logic, not
 * app glue, so it compiles into the sim exactly like the components/ above. */
#include "../../main/beacon_policy_calc.c"
/* Receipt reliability campaign Task 2: the broadcast delivery receipt's
 * slot/jitter/retry policy and packet builder. Same rationale as
 * beacon_policy_calc.c above (pure policy, no ESP-IDF deps, already host-
 * tested directly), and it keeps gosim's receipt storm spread by firmware's
 * real constants instead of a copy of them. */
#include "../../main/broadcast_delivery_receipt.c"
/* Mesh digital twin: the bramble.exportTopology document builder. Compiled in
 * for the same reason as the two above (pure serialization over the real
 * routing tables, no ESP-IDF deps), and for one more: it makes the twin's
 * round-trip check meaningful, because the export gosim writes for a simulated
 * node is written by the firmware's export code rather than by a second
 * implementation of the schema. See ../../docs/digital-twin.md. */
#include "../../main/topology_export.c"
