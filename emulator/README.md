# Bramble Emulator

A virtual Bramble Pager v1: the real firmware compiled for ESP-IDF's linux
target, N virtual pagers on a simulated RF ether (gosim), rendered in a
browser as the physical device. See `DESIGN.md` for the architecture and
`PLAN.md` for the implementation plan.

## Quick start

### Docker (recommended, zero prerequisites)

```
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

```
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

Individual targets, each a thin wrapper documented inline in `Makefile`:

| Target | Wraps |
|---|---|
| `make check` | prerequisite check (`scripts/check_prereqs.sh`) |
| `make node` | `idf.py build` for the linux target (`scripts/build_node.sh`, see `node/README.md`) |
| `make broker` | `go build` in `simulator/gosim` |
| `make ui` | `npm ci && npm run build` in `simulator/ui` |
| `make run` (alias `make serve`) | check + build all, then launch gosim serving the UI and scenarios on `0.0.0.0` |
| `make headless` | the CI scenario suite (`ci/run_scenarios.sh`) |
| `make clean` | remove build artifacts (node build dir/sdkconfig, gosim binary, UI dist) |

Override the port with `make run PORT=3010` if `3000` is already taken on
your machine.

ESP-IDF location defaults to `~/src/esp-idf`; override with `IDF_PATH=...`
if yours lives elsewhere (e.g. `make run IDF_PATH=/opt/esp-idf`).

## Manual build (what the targets above wrap)

```
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

## CI-equivalent scenario suite

```
make headless
```

Wraps `ci/run_scenarios.sh`: boots gosim headless for `emu-channel-delivery`
and `emu-dm-desync`, asserts on rendered e-paper content and firmware log
signatures. Exit code gates CI; budget under 5 minutes.

## Hardware bridge

`bramble-gosim --gateway /dev/ttyUSB0` bridges a real serial-attached
Heltec (PHY passthrough mode) into the ether, so the real mesh and the
virtual pagers share one channel. See `DESIGN.md` section 10 for the gating
rules (disabled by default, authenticated RPC enable, TTL, no persistence).
