# Bramble Emulator

A virtual Bramble Pager v1: the real firmware compiled for ESP-IDF's linux
target, N virtual pagers on a simulated RF ether (gosim), rendered in a
browser as the physical device. See `DESIGN.md` for the architecture. The
original Phase 1 implementation plan is archived at
`docs/archive/plans/emulator-plan.md`.

## Quick start

### The playground (start here if you have no hardware)

```bash
cd emulator
make playground          # or: docker compose --profile playground up playground
```

Open the URL it prints (port 3005 under Docker). It boots a three-pager fleet
of real firmware on a simulated ether and runs a guided tour through
provisioning, a relayed channel message, a direct message with its 7-digit
safety number, and a delivery receipt with the route it came home by. Full
walkthrough and the honest framing: [../docs/playground.md](../docs/playground.md).

### Docker (recommended, zero prerequisites)

```bash
cd emulator
docker compose up --build
```

Open <http://localhost:3004/>. The image bundles ESP-IDF (linux target
only), Go, and Node, and builds the firmware node, gosim, and the UI inside
the container; nothing needs to be installed on the host but Docker itself.
Expect a first build in the 5-10 minute range (mostly the ESP-IDF clone and
the firmware compile) and a final runtime image around 100-150 MB (a
`debian:bookworm-slim` base plus the ~5 MB node binary, ~10 MB gosim binary,
and the small UI bundle; the ESP-IDF toolchain itself is discarded after the
build stage).

### Make targets (local toolchain)

```bash
cd emulator
make check      # verify idf.py (linux target), go, node/npm, jq are present
make run        # build node + gosim + UI, then serve them
```

`make run` prints the URL it serves on (bound to `0.0.0.0`, so it is
reachable from another machine on the LAN, not just `localhost`). Open it,
then load a scenario from the dropdown:

- `emu-channel-delivery`: three provisioned pagers, live broadcast delivery,
  the best one for a first look at a working mesh.
- `emulator-3-pagers`: three unprovisioned pagers (attach/persistence only,
  no traffic).
- `emu-dm-desync`: the DM session desync repro and its self-heal.
- `emu-playground`: three unprovisioned pagers in a line, two of them out of
  range of each other, driven entirely by hand. This is what `make playground`
  loads; add `?tour=1` to the URL to get the guided tour on a stack started
  with `make run`.

Individual targets, each a thin wrapper documented inline in `Makefile`:

| Target | Wraps |
| --- | --- |
| `make check` | prerequisite check (`scripts/check_prereqs.sh`) |
| `make node` | `idf.py build` for the linux target (`scripts/build_node.sh`, see `node/README.md`) |
| `make broker` | `go build` in `simulator/gosim` |
| `make ui` | `npm ci && npm run build` in `simulator/ui` |
| `make run` (alias `make serve`) | check + build all, then launch gosim serving the UI and scenarios on `0.0.0.0` |
| `make playground` | check + build all, then launch gosim on the `emu-playground` scenario with the guided tour (`--playground`) |
| `make headless` | the CI scenario suite (`ci/run_scenarios.sh`) |
| `make clean` | remove build artifacts (node build dir/sdkconfig, gosim binary, UI dist) |

Override the port with `make run PORT=3010` if `3000` is already taken on
your machine. Note that the local simulator (`simulator/gosim`) also defaults
to `3000`, so running both at once needs one of them moved. The Docker
variants do not collide: the simulator publishes 3003 and the emulator 3004.

ESP-IDF location defaults to `~/src/esp-idf`; override with `IDF_PATH=...`
if yours lives elsewhere (e.g. `make run IDF_PATH=/opt/esp-idf`). The
emulator needs ESP-IDF's **linux** target
(`"$IDF_PATH"/install.sh linux`), which is a separate install from the
`esp32s3` one used for real boards; `make check` will tell you if it is
missing.

`bash ci/run_scenarios.sh` runs the compiled firmware but does not build it.
Run `make node` (or `make run`) first, or it reports a missing
`emulator/node/build/bramble-node.elf`.

More first-run stalls are collected in
[../docs/troubleshooting.md](../docs/troubleshooting.md).

## Manual build (what the targets above wrap)

```bash
# 1. firmware node (linux target)
source $IDF_PATH/export.sh
cd emulator/node
idf.py --preview set-target linux   # first time only
idf.py build                        # -> emulator/node/build/bramble-node.elf

# 2. gosim broker
cd simulator/gosim && go build -o bramble-gosim .

# 3. UI
cd simulator/ui && npm ci && npm run build   # -> simulator/ui/dist

# 4. run (broker + UI, real firmware nodes spawned per scenario)
./simulator/gosim/bramble-gosim --ui simulator/ui/dist --scenarios simulator/scenarios
# headless: ./simulator/gosim/bramble-gosim --headless --scenario simulator/scenarios/emu-channel-delivery.json
```

See `node/README.md` for the linux-target build details and known IDF-linux
caveats, and `../simulator/README.md` ("Emulator scenarios" section) for the
scenario schema, per-node env knobs (`EMU_NETWORK_KEY`, `EMU_AUTO_SEND`,
...), and the screen-assertion vocabulary CI gates on.

## The device view

After `make run`, open the URL and use the scenario dropdown to load
`emu-channel-delivery` (three provisioned pagers that exchange a real channel
message), then open the **DEVICES** tab. Each firmware node renders as the
physical pager: a faithful SSD1680 e-paper panel (real partial/full refresh
behavior, seeded from the GDEY0213B74 datasheet), clickable SEL/UP/DN/RST
buttons that send real input to the firmware, LED/vibra indicators, and a
live per-node console. `emu-dm-desync` demonstrates the DM session self-heal;
`emulator-3-pagers` is a plain attach/boot/persistence smoke.

Nodes boot unprovisioned (INERT) unless keyed up. The scenarios seed a shared
network key via the `EMU_NETWORK_KEY` env at boot (the same 32-byte blob on
every node) so they can send and receive channel traffic; per-node identity
stays unique. Provisioning uses the real `network_key_set_provisioned` path,
not a MAC-bypassing shortcut.

## Task priorities are real (issue #50)

The linux target runs with the firmware's REAL task priorities, the same as
device builds. An earlier workaround (`emu_task_flatten.c`) flattened every task
to one priority to dodge an intermittent scheduler-corruption crash that only
appeared under real priorities; it has been removed now that the crash is
root-caused and fixed.

The crash was not a priority bug. The IDF linux port drives its tick as a
process-directed SIGALRM that must be handled only by the running task thread;
every other thread must block signals. The emu_link reader pthread masks signals
as its first act, but it was created (`components/emu_link/emu_link.c`) from a
running FreeRTOS task without the parent blocking signals first, so it was born
with an inherited unblocked mask. In the startup window before it reached its
own masking call, a tick could be delivered to that raw thread and run the port
scheduler on it, corrupting current-task state and aborting via a
priority-inheritance assertion. Flattening hid this (with flat priorities there
is no priority inheritance to assert on); real priorities exposed it. The window
is microseconds but widens under the CPU starvation of a shared CI runner, which
is why it read as a rare, load-amplified "mute node" death.

The fix blocks all signals in the parent around `pthread_create` so the reader
inherits a fully-masked signal set from birth, closing the window. With it, real
priorities run correctly (full scenario suite green, including under single-core
CPU starvation), and the emulator now matches device scheduling instead of
hiding priority-design bugs. Rollback lever: git history has `emu_task_flatten.c`
if flattening is ever needed again as a scoped workaround.

## CI-equivalent scenario suite

```bash
make headless
```

Wraps `ci/run_scenarios.sh`: boots gosim headless for `emu-channel-delivery`
and `emu-dm-desync`, asserts on rendered e-paper content and firmware log
signatures. Exit code gates CI; budget under 5 minutes.

## Browser end-to-end suite

```bash
make e2e
```

Drives a real headless Chromium through the full stack (Playwright) and
asserts what a user actually sees: the device-view canvas pixels equal the
firmware framebuffer for a delivered message (independent decode, pixel-exact),
the full-refresh flash starts black before content, exactly N cards for N
nodes, button clicks reach firmware with a visible reaction, RESET restarts a
node with the same identity, and a channel message from node A renders on node
B's e-paper. Wired into CI as a non-required job first. Teardown is scoped to
the run's own gosim process group, so it will not disturb a separate
`make run` serve on another port.

## Hardware bridge

`bramble-gosim --gateway /dev/ttyUSB0` bridges a real serial-attached
Heltec (PHY passthrough mode) into the ether, so the real mesh and the
virtual pagers share one channel. See `DESIGN.md` section 10 for the gating
rules (disabled by default, authenticated RPC enable, TTL, no persistence).
