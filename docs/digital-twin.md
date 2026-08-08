# Mesh digital twin

Import a running deployment's observed topology into the simulator and ask
capacity and resilience questions about that mesh, rather than about a
hypothetical one.

Each node can hand you its own view of the mesh in a single RPC call
(`bramble.exportTopology`). Collect one document per node, feed the files to
`bramble-gosim twin`, and the simulator merges them into a link graph, rebuilds
the deployment as a runnable scenario, and runs the real protocol code over it
to answer two questions:

- **Capacity.** Ramp the offered message rate and watch end-to-end delivery.
  Where delivery falls away is where the deployment's usable message rate is.
- **Criticality.** Remove each node in turn and report what the mesh breaks
  into. That is the list of single points of failure, worst first.

## What the twin does and does not reproduce

The reconstruction is built from **reported reachability**, not from geometry.
A device can tell you which neighbours it heard and at what RSSI and SNR; it
cannot tell you where it is, and inverting RSSI back into a position would
invent propagation physics nobody measured. So the twin takes the observations
themselves as the model: a directed link graph, with the simulator's radio
reading audibility and received power out of that graph instead of out of node
coordinates.

Reproduced:

- Which nodes can hear which, in each direction, as the fleet reported it.
- The RSSI and SNR each link was heard at.
- The PHY that prices time-on-air (SF, bandwidth, coding rate) and the
  frequency plan's duty-cycle ceiling.
- Everything the simulator already models on top of audibility: real LoRa
  time-on-air, collisions, the capture effect, half-duplex, and
  listen-before-talk, all running the firmware's own protocol code.

Not reproduced, and not claimed:

- **Propagation.** The twin replays observed links. It cannot tell you whether
  a link survives rain, a new building, or a different antenna.
- **A moment other than the export.** Every link is a snapshot taken when the
  call was made. A mesh whose links vary with the weather has a different twin
  each time you export it.
- **Nodes that are not deployed.** There is no placement mode, because placing
  a node requires positions and predicted propagation, and the export carries
  neither. The twin answers rate-capacity and node-loss questions about the
  mesh you have.
- **Field results.** Every number the report prints is simulation, over a
  reconstruction, and the report says so at the top of its own output.

## Workflow

### 1. Export from each node

`bramble.exportTopology` is an authenticated query, so it takes the same
credentials as any other RPC (`docs/auth.md`). Save the reply for each node:

```bash
printf '%s\n' '{"jsonrpc":"2.0","id":1,"method":"bramble.exportTopology","params":{}}' \
  | websocat -n1 "ws://192.0.2.179/ws?token=$TOKEN" > tower.json
```

The importer accepts either the whole JSON-RPC response or just the `result`
object, so "save whatever the call returned" is a working instruction.

Export from as many nodes as you can reach. Every node that exports replaces
one-sided evidence with a measured link: for a node that never exports, only
the direction its neighbours heard is known, and the report names every
direction it had to assume.

### 2. Run the twin

```bash
cd simulator/gosim && go build -o bramble-gosim .
cd .. && ./gosim/bramble-gosim twin gosim/testdata/twin/*.json
```

Flags:

| Flag | Meaning |
| --- | --- |
| `-rates 1,2,5,10,20,40` | The offered-load ramp, in messages per minute. Must ascend. One full scenario run per rate. |
| `-duration-ms 600000` | Observation window of each capacity run. The default matches the window `docs/results/simulation-2026-07-honest-baseline.md` measures in. |
| `-seed 1` | PRNG seed for every scenario the probe runs. |
| `-skip-capacity` | Report topology and criticality only, skipping the probe's per-rate runs. |
| `-scenario PATH` | Write the reconstructed scenario out, to run or edit by hand. |
| `-json PATH` | Also write the machine-readable report (`-` for stdout). |

Progress lines go to stderr; the report goes to stdout.

### 3. Read the report

The example below is the committed sample export set under
`simulator/gosim/testdata/twin/`, which is what the tests in
`simulator/gosim/twin_import_test.go` read, so this worked example and the
tested behaviour cannot drift apart. Four of the five nodes exported. The whole
example, six full 600-second scenario runs, completes in under a second.

```text
$ ./gosim/bramble-gosim twin gosim/testdata/twin/*.json
Bramble mesh digital twin
=========================

Every number below is simulation: the reconstructed mesh run through the
firmware's own protocol code and the simulator's collision model, not a
field measurement. The twin replays the links these nodes reported at the
moment of export; it does not predict propagation.

Imported mesh
-------------

4 export file(s), 5 node(s), 8 directed link(s).
4 of 5 nodes exported their own view.

Radio: SF9 BW125000 CR4/5, 915.0 MHz, US915 (FCC Part 15.247)
Duty cycle: 100%, advisory, so the twin applies no regulatory cap.

Nodes:

  address    name               exported  links    firmware
  0A1B2C3D   basecamp           yes       1        0.9.3
  1B2C3D4E   ridge              yes       2        0.9.3
  2C3D4E5F   creek              yes       1        0.9.3
  3D4E5F60   tower              yes       3        0.9.3
  4E5F6071   north-cabin        no        1        -

Observed links
--------------

  from       to           rssi   snr  source
  0A1B2C3D   3D4E5F60      -95     8  observed
  1B2C3D4E   3D4E5F60      -99     5  observed
  1B2C3D4E   4E5F6071      -88    11  assumed reciprocal
  2C3D4E5F   3D4E5F60     -110     1  observed
  3D4E5F60   0A1B2C3D      -92     9  observed
  3D4E5F60   1B2C3D4E     -101     4  observed
  3D4E5F60   2C3D4E5F     -108     2  observed
  4E5F6071   1B2C3D4E      -88    11  observed

Reconstruction gaps
-------------------

Nodes present only through other nodes' neighbour tables (1): 4E5F6071
  Export from these to replace one-sided evidence with measured links.

Link directions nobody reported, assumed reciprocal (1):
  1B2C3D4E -> 4E5F6071 at the reverse direction's -88 dBm / 11 dB
  A real one-way link would make the mesh worse than the twin shows.

Capacity probe (simulation)
---------------------------

Offered load ramped over a 600 s window, seed 1, one run per rate.
Traffic is the same construction the published scale runs use: one message
at a time, round-robin source, destination half the fleet away. Delivery is
message_delivery_rate (reached the destination); confirmed is
confirmed_delivery_rate (the receipt made it back to the sender). Each row's
percentages are over that row's message count, so the bottom of the ramp is
the smallest sample on the table.

    msgs/min  messages delivered  confirmed   erlangs chan util   control   latency
           1        10       40%        40%      0.08      7.8%     96.2%      161ms
           2        20       55%        55%      0.11     10.6%     90.1%      205ms
           5        49       65%        65%      0.20     19.5%     86.5%      171ms
          10        97       67%        66%      0.26     26.3%     76.5%      296ms
          20       194       72%        72%      0.33     32.9%     57.4%      587ms
          40       387       67%        62%      0.43     43.1%     43.2%      778ms

Saturation knee: not reached. Delivery peaks at 72% (20 msgs/min) and
stays within 90% of that all the way to 40 msgs/min, the highest rate
probed. Ramp higher to find the knee.

Delivery is also below that bar at the BOTTOM of the ramp (1, 2 msgs/min),
which is not saturation. A mesh carrying almost no traffic lets its routes
expire between messages, so each message pays for a fresh discovery flood;
those runs also carry the fewest messages, so they are the noisiest rows.

Node criticality (simulation)
-----------------------------

With every node present the mesh is one connected piece.

Removing each node in turn:

  address    name                links  pieces  cut off from the rest
  3D4E5F60   tower                   3       3  0A1B2C3D, 2C3D4E5F
  1B2C3D4E   ridge                   2       2  4E5F6071
  0A1B2C3D   basecamp                1       1  nothing
  2C3D4E5F   creek                   1       1  nothing
  4E5F6071   north-cabin             1       1  nothing

2 node(s) are single points of failure: losing any one of them strands
the nodes listed beside it. Adding a link that bypasses one is the
cheapest resilience the twin can point at.
```

What an operator takes from that: the hub carries three of the four links and
its loss splits the mesh into three pieces, so the cheapest resilience buy is a
link that bypasses it. The mesh holds delivery up to at least 40 messages per
minute, which is the top of the default ramp rather than a ceiling anybody
found, and the report says which it is instead of implying a limit. And a fifth
node never exported, so one link direction in the model is an assumption rather
than an observation.

## Reading the numbers honestly

- **Every figure is simulation.** Reconstructed topology, real firmware
  protocol code, the simulator's collision model. Not a field measurement.
- **Delivery is a rate over that row's messages.** The bottom of the ramp
  scripts the fewest messages and is the noisiest row on the table.
- **Delivery against offered load does not have to fall monotonically.** A mesh
  carrying almost no traffic lets routes expire between messages, so each
  message pays for a fresh discovery flood; the report finds the knee from the
  ramp's peak and names any low-rate rows that fall short separately, rather
  than calling the quietest run "saturated".
- **Criticality is connectivity, not capacity.** A node whose removal strands
  nothing may still carry most of the traffic. Read the criticality table with
  the link counts beside it.
- **A partitioned import is reported before anything is removed**, so every
  removal row is read against a mesh that was already in pieces.

## Machine-readable output

`-json PATH` writes the same analysis as JSON: the merged nodes and links, the
PHY, the reconstructed scenario, the criticality sweep, the capacity ramp, and
an `assumptions` block that restates the bounds in fields rather than prose:

```json
{
  "kind": "simulation over an observed link snapshot; not a field measurement and not a propagation prediction",
  "reciprocal_links_assumed": 1,
  "one_way_links": 0,
  "unexported_nodes": 1,
  "observation_window_ms": 600000
}
```

## How the reconstruction works

- **The export document** is built by `main/topology_export.c`, which also
  writes the arrays `bramble.getNeighbors` and `bramble.getRoutes` return, so
  an export cannot disagree with the two methods it subsumes. The schema is
  `TopologyExportResponse` in `api/openapi.yaml`, versioned by `twin_schema`;
  the importer refuses a version it does not know rather than guessing at
  fields.
- **The merge** (`simulator/gosim/twin_graph.go`) reads each neighbour entry as
  a directed observation: the RSSI was measured at the exporting node, so it
  describes the neighbour-to-exporter direction. Two documents describing the
  same direction resolve to the fresher reading, and the merge notes it.
  Reciprocity fills the missing direction of a link only when the node at the
  far end never exported, so nobody could have reported that direction: leaving
  it out would model a one-way link no protocol exchange can cross and report a
  mesh far more broken than the one that is running. When the far end did
  export and its neighbour table does not name the transmitter, that is a
  device reporting that it does not hear it, and the direction stays out: the
  twin carries the link as one-way, the report names it, and the partition
  traversal (`radio_nodes_connected`, which requires both directions) treats
  the two ends as unconnected. Exports from nodes on genuinely different PHYs
  are refused, since one twin models one channel.
- **The scenario** (`simulator/gosim/twin_scenario.go`) is an ordinary gosim
  scenario carrying a `links` block, which puts the radio into link mode
  (`radio_config_t` in `simulator/engine/sim_radio.h`). In link mode audibility
  and received power come from the imported table and node coordinates carry no
  physical meaning, so the twin lays nodes out on a ring purely so the
  simulator UI draws something legible. Nothing else about the model changes.
- **Criticality** runs `anomaly_partition_components`, the same traversal the
  shipped `mesh_partition` detector uses
  (`docs/bramble-anomaly-detection.md`), so "still connected" has one
  definition in the product and in the twin.
- **Capacity** scripts traffic with the construction
  `simulator/scenarios/generate.py` uses for the published scale runs, so a
  twin's delivery-against-offered-load curve is read on the same terms as
  `docs/results/simulation-2026-07-honest-baseline.md`.

## Verification (simulation)

The pipeline is verified end to end by round-trip, in
`simulator/gosim/twin_roundtrip_test.go`:

1. A four-node line scenario runs, and its nodes learn each other by beacon
   exactly as firmware does.
2. Every node exports through `main/topology_export.c`, the same builder the
   firmware RPC calls. The simulator compiles that file rather than
   reimplementing the schema, so what is re-imported is written by firmware
   code.
3. The documents are re-imported, and the reconstructed link graph is compared
   against the ordered node pairs the radio model itself says could carry a
   frame. They match exactly: six directed links for the four-node line, none
   invented and none dropped.
4. The reconstruction is then re-simulated as a twin scenario and exported
   again, and the second graph matches the first link for link, including every
   RSSI and SNR. The SNR column is checked to be the link quality the radio
   actually computed (RSSI above a -120 dBm noise floor, within the model's
   2 dB of jitter) rather than a constant, so that comparison is between two
   populated vectors.

That verifies the fidelity of the export, merge, scenario and re-export path.
It does not verify the simulator's propagation against a real hillside, which
is why every twin number is labelled as simulation. There is no field
measurement of a twin's capacity prediction against the deployment it was built
from.

## Scope

The twin is the `twin` subcommand and its report. There is no web client
surface for it, no `bramble-cli` support, and no hypothetical-node placement.
