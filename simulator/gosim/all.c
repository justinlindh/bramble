/*
 * all.c — Single compilation unit that includes all C sources for cgo.
 * cgo compiles this as one .o file. bridge.c is compiled separately by cgo.
 */

/* ESP stubs first */
#include "../../test/stubs/esp_stubs.h"

/* No-op NVS stubs for components that persist settings (location.c) */
#define NVS_STUBS_ENABLE 1
#include "../../test/stubs/esp_stubs.c"

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
/* routing_auth.c + identity_store.c live in identity_unit.c, a separate
 * compilation unit: routing_auth.c's static ct_eq collides with
 * discovery.c's in this single-TU build. */

/* New components (Phase 6) */
#include "../../components/mailbox/mailbox.c"
#include "../../components/location/location.c"
#include "../../components/channel/channel_key.c"
#include "../../components/channel/public_channel.c"

/* Firmware policy modules: pure, no ESP-IDF deps, host-tested directly
 * (test/test_beacon_policy_calc.c). Lives under main/ but is logic, not
 * app glue, so it compiles into the sim exactly like the components/ above. */
#include "../../main/beacon_policy_calc.c"
