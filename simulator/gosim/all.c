/*
 * all.c — Single compilation unit that includes all C sources for cgo.
 * cgo compiles this as one .o file. bridge.c is compiled separately by cgo.
 */

/* ESP stubs first */
#include "../../test/stubs/esp_stubs.h"

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
