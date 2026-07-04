# Phase 2 (scale) progress ledger
Roadmap: ~/src/bramble-meta/plans/2026-07-04-scalable-mesh-roadmap.md
Framework: ~/src/bramble-meta/plans/2026-07-04-phase2-scale-framework.md
Baseline: docs/results/simulation-2026-07-honest-baseline.md (+ Phase 1 addendum). TRUE-parity: n=10 50%, n=50 12%, n=100 ~2%.
Base main: 372aadfa (post Phase 1 #124). Branch: feat/phase2-flood-comparison.
OWNER: radio knobs (SF/BW/CR/beacon-cadence/tx-power/channel) are SHIPPABLE default-change levers, not just advisory.
Phase 2 is MEASUREMENT-DRIVEN: each lever gets before/after numbers in the honest sim; accept only if it measurably helps 50/100 w/o regressing 10; ship accepted levers. BAR: >=90%@50, >=80%@100 delivery-with-confirmation.
TASK 0 (first, before levers): flood-comparison baseline = Meshtastic-style managed flooding in gosim, head-to-head vs Bramble reactive routing on identical physics. Answers "how do we compare" + tests the CENTRAL THESIS (reactive AODV vs flooding at 50-100 nodes). Flooding-wins -> D2-class finding.
Verify: cd simulator/gosim && go build ./... && go test -count=1 ./...; host suite green if firmware touched.
Task 0 (flood comparison): DONE (commit cd7daa71, flood.go Go-only, controller-VERIFIED via own runs). Scenario field "routing":"flood" + "flood_hop_limit"; flood emits flood_final_metrics/flood_reached/flood_confirmed events.
=== TWO VERIFIED FINDINGS (BOTH re-run by controller) ===
FINDING 1 (THESIS FALSIFIED): managed flooding BEATS reactive AODV at every node count on identical physics. My runs: n=10 reactive ~50% vs flood 100%/100% (reached/confirmed); n=100 reactive ~0% vs flood hl=7 90%/90%. WHY: flood is BEACON-FREE + DISCOVERY-FREE (no control plane); reactive's RREQ discovery + beacons bankrupt the channel (>1.0 erlang from discovery alone at 50). At LoRa airtime economics a control plane may not pay for itself vs stateless flooding. Flood even gets 90% CONFIRMED at 100 nodes (flooded-ACK) vs reactive 0%. hl=3 (Meshtastic default) fails at 50+ on grid diameter (topology, not algorithm); hl=7 (~Bramble's HOP_LIMIT_MAX 8) is the fair basis.
FINDING 2 (METRIC MISLABEL): the message_delivery_rate the WHOLE PROGRAM cited as "delivery-with-confirmation" actually fires at DESTINATION RECEIPT (bridge.c:825 "don't wait for receipt to arrive at source"), NOT sender-confirmation. Reactive's TRUE confirmed-delivery: 40% @ n=10 (vs 50% cited), 0% @ n=50/100. -> baseline doc needs an honesty correction pass (with-confirmation -> destination-reach for reactive at scale).
=== D2 DECISION POINT REACHED. This is an OWNER call (architectural pivot). Presenting. Caveats: sim of Meshtastic-STYLE strategy not the firmware; at Bramble's current radio defaults; flood is structurally beacon-free so benefits less from beacon levers but reactive's discovery cost is the killer regardless. ===

=== "SAVE REACTIVE" MEASUREMENT ROUNDS (controller, direct sim, SF7 is the story) ===
BEACON CADENCE lever: USELESS for delivery. 60s->900s beacons cuts beacon ToA 321s->32s at n=50 but delivery flat (10%->5%); RREQ discovery (682s->709s) saturates the channel ALONE. Beacons are not the bottleneck; DISCOVERY is.
Legacy traffic is MAXIMALLY COLD (20 msgs / 20 distinct dests / 20 distinct pairs -> every msg a fresh discovery). Warm-route test confounded (single N001->N027 discovery just fails+refloods).
KEY INSIGHT: reactive's RREQ discovery IS a flood. Cold routes -> reactive = flood(discovery)+unicast(data)+beacons > flood(data) alone. Structurally can't beat flooding for cold routes at SF10.
*** SF7/250k RADIO KNOB (owner-unlocked) SAVES REACTIVE: n=50 10%->45% (rreq 682s->60s, erl 1.71->0.15 = OUT of saturation); n=100 ~0%->65%. 13x airtime cut lets discovery COMPLETE. ***
HEAD-TO-HEAD at SF7/250k (reactive reach vs flood reach/confirm): n=50 45% vs 60%/30%; n=100 65% vs 90%/90%. Flood still leads REACH by ~15-25pts but reactive is now COMPETITIVE (was obliterated) + UNSATURATED (erl 0.15-0.20 = huge headroom).
RISING-LOAD (SF7, n=50, 2->60 msg/min): both HOLD, neither collapses. reactive 45->71->70->66% (erl 0.15->0.81); flood steady 75%/63%. Flood keeps modest reach lead even at load; reactive did NOT clearly overtake on efficiency in this test.
STATUS: reactive is SALVAGEABLE (SF7 = the accepted lever, ship it as default candidate). But flood still modestly leads reach at SF7. Open: reactive's TRUE confirmed-delivery at SF7 (message_delivery_rate is REACH per Finding 2); intermediate-node RREP (firmware lever, cut discovery flood depth); whether reactive's airtime-efficiency wins at higher load/node counts than tested. CHECKPOINT to owner.

=== CRITICAL CONFOUND FOUND (controller, before building on SF7): SIM DECOUPLES SF FROM RANGE ===
sim_radio.c: reception is a HARD DISK (config->range=150 fixed, dist>range gate at :103) INDEPENDENT of SF. SF only affects airtime (bramble_calculate_airtime_us), never the range disk. So the SF7 test gave reactive the ~13x airtime win with ZERO range penalty = UNREALISTIC and likely INVERTS the result: real SF7 has ~9dB less budget -> ~2x shorter range (~73u) -> at 120u legacy grid spacing the mesh DISCONNECTS -> ~0% not 45%. "SF7 saves reactive" is NOT trustworthy, possibly wrong.
This is the Phase-0 honest-ruler lesson in a new place: airtime is honest, SF-to-range coupling is NOT modeled. Radio knobs (owner's key lever) cannot be trusted in the sim until fixed.
REVISED PLAN (owner chose intermediate-RREP + true-confirmed; adjusting execution to be honest):
1. PREREQ: fix sim SF/BW -> link-budget -> range coupling (make radio-knob ruler honest). Range must derive from sensitivity(SF,BW), not a fixed 150. THEN re-test SF7 honestly (may need tighter spacing to connect = real deployment finding).
2. intermediate-node RREP (SF-INDEPENDENT discovery-flood-depth lever, valid regardless) + measure reactive TRUE confirmed-delivery (not reach; message_delivery_rate is reach per Finding 2).
All prior SF7/BW numbers (45%/65%/load-test) are SUSPECT pending the coupling fix. SF10 numbers stand (SF10 is the modeled default).

=== SF-RANGE COUPLING FIX DONE (commit 30603bdc, controller-verified) + HONEST SF FINDING ===
sensitivity(sf,bw) = base_sens(sf datasheet) + 10log10(bw/125k) + NOISE_MARGIN 38.9dB (calibrated SF10/125k->149.9u exact). Derived: SF7/250k=57.8u, SF12/125k=223.1u. Legacy scenarios all set range EXPLICITLY -> zero baseline shift. SF7/250k @120u legacy grid = 0% (DISCONNECTED, was 65% decoupled). "SF7 saves reactive" DEBUNKED at fixed topology.
HONEST NUANCED FINDING (50 nodes): SF10/125 @120u -> reactive 10%; SF7/250 @45u (dense, connects, receptions_ok=6310) -> reactive 60% (erl 0.16, rreq 60s), MATCHES flood reach 60% + far more airtime-efficient. SO: SF7 helps reactive BUT requires ~2.6x denser node placement (range/airtime/density tradeoff). SF = deployment param matched to density, not a universal knob. Reactive is VIABLE + EFFICIENT in dense deployments; the efficiency-thesis has honest footing there.
=== NEXT (owner's choice): intermediate-node RREP (SF-independent discovery lever) + reactive TRUE-confirmed metric. Measure before/after at SF10/120u AND SF7/45u-dense vs flood. Fair efficiency re-test (rising load, honest range) belongs here too. ===

=== MILESTONE (verified 3 seeds by controller): REACTIVE BEATS FLOOD ON BOTH BARS at the right operating point ===
Commits b8caaab7 (confirmed_delivery_rate metric), d3fd2185 (intermediate-node RREP, DISCOVERED-only + 60s-fresh + auth'd + no-double-forward), 761efb4c (report). 92/92 host, gosim green, board clean, rpc 51/51.
SF7/250 @45u dense, 50 nodes, 3 seeds: reactive reach 60-75% / CONFIRMED 55-65% vs flood reach 60% / confirmed 30%. Reactive at erl 0.15-0.17 (vs flood's flooding). Intermediate-RREP: SF7/45u reach ~30%->65% (RREQ airtime -8.5%); SF10/120u UNCHANGED (0%, channel so jammed only 1 of 20 msgs ever originates a discovery -> nothing to short-circuit; the lever helps where there's HEADROOM, not in deep saturation).
STRUCTURAL FINDING: reactive's 2x confirmation advantage (60% vs 30%) is Bramble's differentiator made real: route-maintenance gives the receipt a path home; stateless flooding's flooded-ACK is unreliable. Reactive wins WHEN correctly deployed (SF matched to density); flood is more FORGIVING of misconfiguration (60% even at wrong SF10/120u where reactive=0%).
=== REACTIVE BET VINDICATED for Bramble's niche = confirmed delivery in a tuned dense mesh, ~6x less airtime than flooding. Operating point (SF<->density match) is the master variable. Checkpoint to owner. ===

=== STEP 1 RESULT (controller, dense SF7/250 @45u, honest radio, 3 seeds): CROSSOVER, advantage does NOT generalize ===
nodes -> reactive reach/conf vs flood reach/conf:
  n=25: reactive ~85/75  vs flood 65/35   -> REACTIVE wins both
  n=50: reactive ~67/60  vs flood 60/30   -> REACTIVE wins both
  n=75: reactive ~42/38  vs flood 80/80   -> FLOOD wins both
  n=100: reactive ~60/55 vs flood 90/90   -> FLOOD wins both
CROSSOVER at ~60-75 nodes. Flood confirmation RISES with node count (35->30->80->90: density gives redundant ACK paths); reactive DEGRADES with hop count (multi-hop round-trip loss). reactive erl only ~0.2 at n=50/100 = NOT saturation-limited; reactive's ~65% reach ceiling at dense-unsaturated is a ROUTING-RELIABILITY wall (35% of msgs never form/complete a route even with airtime to spare) -> maybe fixable (robust retry/discovery) or fundamental (multi-hop round-trips over lossy links).
HARD TRUTH: NEITHER pure strategy meets the program bar (90%@50, 80%@100 CONFIRMED). Reactive tops ~60% conf at 50; flood only 30% conf at 50 (good reach, bad small-N ACK), 90% at 100.
Reactive's niche NARROWER than the 50-node milestone suggested: wins only <=~50-60 nodes. D2/kill-criteria adjacent. OWNER DECISION needed.
Open sub-question: flood's small-N confirmation (30-35%) may be TUNABLE (flooded-ACK hop-limit/suppression) -> if so flooding could win the whole range.

=== SUBSTRATE QUESTION RESOLVED (controller, 3 seeds): FLOODING WINS THE WHOLE RANGE ===
Root cause of flood's small-N weakness: floodSuppressAfterHeard=1 (Meshtastic default) cancels a node's rebroadcast after hearing just 1 copy -> in a SPARSE mesh kills coverage before the flood/ACK propagates. Raising to 2 fixes it with NO large-N cost (dense meshes still hear 2+ copies fast).
flood(suppress=2) vs reactive(+intermediate-RREP, its best levers), dense SF7/250 @45u, 3 seeds, reach/confirmed:
  n=25: flood 100/100 vs reactive ~83/77
  n=50: flood 80/80  vs reactive ~67/60
  n=75: flood 80/80  vs reactive ~42/38
  n=100: flood 90/90 vs reactive ~60/55
FLOOD WINS EVERY node count on BOTH bars. Nearly clears program bar (conf 80%@50 vs 90 target, 90%@100 vs 80 target). Reactive's small-mesh niche EVAPORATES when flood is tuned.
CONCLUSION: adopt FLOODING as the substrate. Reactive AODV (RREQ/RREP/breadcrumbs/intermediate-RREP) is falsified even in its niche. Bramble's differentiator (AUTHENTICATED + CONFIRMED-delivery flooding) sits ON TOP: flooded-ACK gives 80-100% confirmation; the DATA auth_hmac + security layer apply regardless of substrate. Meshtastic floods but weak-security + fire-and-forget; "authenticated confirmed-delivery flooding" is the demonstrated niche.
CAVEAT: all in-sim (Meshtastic-STYLE model). Adopting = BUILD a flooding transport in firmware (real project). Reactive stack would deprecate. suppress=2 is a Bramble tuning (deviates from Meshtastic default, justified). Owner PIVOT decision needed.
