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
#include "../../components/routing/routing.c"
#include "../../components/routing/discovery.c"
#include "../../components/routing/forwarding.c"
#include "../../components/packet/packet.c"
#include "../../components/reliability/reliability.c"
#include "../../components/dedup/dedup.c"
#include "../../components/airtime/airtime_budget.c"
#include "../../components/fragment/fragment.c"
#include "../../components/crypto/crypto_host.c"

/* New components (Phase 6) */
#include "../../components/mailbox/mailbox.c"
#include "../../components/emergency/emergency.c"
#include "../../components/location/location.c"
#include "../../components/group/group.c"
#include "../../components/coding/coding.c"
#include "../../components/routing/route_metric.c"
#include "../../components/channel/channel_key.c"
#include "../../components/channel/public_channel.c"
