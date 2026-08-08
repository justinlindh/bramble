# Troubleshooting

Things that commonly stall a first build, flash, or test run, and what to do
about them. If you hit something that is not here and you work out the fix,
adding it to this file is a welcome pull request.

- [Paths and prerequisites](#paths-and-prerequisites)
- [Serial and flashing](#serial-and-flashing)
- [Firmware updates over the air](#firmware-updates-over-the-air)
- [Simulator and emulator](#simulator-and-emulator)
- [Tests and quality gates](#tests-and-quality-gates)
- [Commits and CI](#commits-and-ci)

## Paths and prerequisites

### `export.sh: No such file or directory`, or `idf.py: command not found`

`scripts/flash.sh` and the emulator both need an activated ESP-IDF v5.4.1
environment. `scripts/flash.sh` searches `$IDF_PATH` first, then
`~/src/esp-idf`, `~/esp-idf`, `/opt/esp/idf`, and `/opt/esp-idf`. The emulator
scripts use `$IDF_PATH` and fall back to `~/src/esp-idf`. If your checkout is
somewhere else, set `IDF_PATH` to point at it:

```bash
export IDF_PATH=/path/to/your/esp-idf
source "$IDF_PATH/export.sh"
```

The emulator's make targets take it inline as well:

```bash
make run IDF_PATH=/opt/esp-idf
```

Nothing in the build requires ESP-IDF to live at any particular path. See
[BUILDING.md](BUILDING.md) for the full setup.

### The ESP-IDF Python virtualenv is not picked up

If `idf.py` runs but immediately complains about missing Python packages, the
IDF virtualenv is probably not on your `PATH`. `export.sh` normally handles
this; when it does not:

```bash
IDF_VENV=$(ls -d "$HOME/.espressif/python_env"/idf*.4_py*_env 2>/dev/null | sort -V | tail -1 || true)
if [[ -n "${IDF_VENV:-}" ]]; then
  export PATH="$IDF_VENV/bin:$PATH"
fi
```

### The emulator says ESP-IDF is present but the build fails

The emulator compiles the firmware for ESP-IDF's **linux** target, which is a
separate toolchain install from the ESP32-S3 one. Installing only `esp32s3` is
the single most common emulator setup failure:

```bash
# for firmware you will flash to a board
"$IDF_PATH"/install.sh esp32s3

# for the emulator (and you can install both)
"$IDF_PATH"/install.sh linux
```

`bash emulator/scripts/check_prereqs.sh` checks all of this and names what is
missing. Run it before anything else in `emulator/`.

## Serial and flashing

### Permission denied opening `/dev/ttyUSB0` or `/dev/ttyACM0` on Linux

Your user is not in the group that owns the serial device. Check which group
that is:

```bash
ls -l /dev/ttyUSB0
```

The group in that listing is typically `dialout` on Debian, Ubuntu, and
Fedora, and `uucp` on Arch. Add yourself to it:

```bash
sudo usermod -aG dialout "$USER"   # substitute the group you saw above
```

Then log out and back in, or run `newgrp dialout` in the shell you are using.
Group membership is applied at login, so it will not take effect in an
existing session on its own.

Running the flash script under `sudo` is not a fix. It works once and then
leaves root-owned files in your build directory that break the next
non-`sudo` build.

### The device does not appear at all

- Try a different USB cable. Charge-only cables are extremely common and
  present no serial device at all.
- `dmesg | tail` right after plugging in will say whether the kernel saw it.
- Boards with native USB (T-Deck Plus, Bramble Pager v1) enumerate as
  `/dev/ttyACM*`; boards with a USB-serial bridge (Heltec V3, V4) as
  `/dev/ttyUSB*`.
- If a serial monitor is still attached from a previous session it holds the
  port open. Close it before flashing.

### Flashing itself

Flash real hardware only through `scripts/flash.sh` or `scripts/flash-all.py`.
They apply the right board defaults, build directory, and sdkconfig. Raw
`esptool` invocations skip that and have bricked devices in the past.

## Short range, and checking transmit power

`bramble config get` reports the power the node was told to use. That is
intent, not evidence. Neither the commanded nor the radiated output power can
be read back from an SX1262: SetTxParams and SetPaConfig are write-only
op-codes, and no register reports output power. Confirming the level actually
radiated needs external instrumentation such as an SDR or a power meter.

What the chip does report is in `bramble.getDiagnostics` under `radio_health`,
and in the boot trace as `Radio health (init)` lines:

- `pa_fault` true means the power amplifier did not ramp for a transmit, so
  nothing usable went on air. This is the strongest on-chip signal that
  commanded power is not being produced.
- `pll_fault` and `oscillator_fault` also stop transmission, they are just
  rarer than a PA fault rather than milder: with no synthesizer lock there is no
  frequency to sit on, and with no reference oscillator there is no clock at
  all, so nothing usable leaves the antenna either way.
- `calibration_fault` is the quiet one. It costs real link budget while every
  later command still succeeds, so it will not show up as a failure anywhere
  else.
- `config_verified` false means configuration writes are not reaching the chip
  at all, which caps output well below the commanded level.
- `detail` carries the chip-specific values behind those verdicts, such as the
  decoded error flag names and the PA settings in use. It is for reading, not
  parsing.

### Comparing nodes on the bench

Reciprocal RSSI isolates a transmit or receive fault without needing to know
distances. For a pair of nodes, path loss and antenna gains are the same in
both directions, so they cancel: read the RSSI each node reports for the other
and subtract. A balanced pair differs by a few dB. A large asymmetry points at
one node's transmit or receive path, usually an antenna or connector.

The method is blind to a fault common to every unit, because a shared fault
cancels out of every pairwise comparison. Only absolute measurement catches
that case.

### Two traps when measuring signal strength

- Neighbour RSSI from `bramble peers` refreshes **only** on beacon reception,
  not on probes or data traffic. Re-reading it in a loop returns the same
  cached value, which looks like a convincingly flat measurement. Gate every
  reading on `last_seen_ms` proving the beacon arrived after whatever you
  changed. Traffic events (`bramble.onTrafficEvent`) carry a per-packet `rssi`
  with the origin in `src_addr`, which is the per-packet alternative.
- Probe-response RSSI belongs to whoever transmitted the frame that arrived. A
  response relayed through another node (`hops` greater than 1) carries the
  relay's signal, not the peer's. Filter on `hops`.

Changing power with `bramble config set-radio --txpower` persists to NVS, so a
sweep leaves the node on its last value. Restore it explicitly.

## Firmware updates over the air

The full journey, including every state the web client shows, is
[updating-your-node.md](updating-your-node.md). The three failures that send
people here:

### "OTA rejected: image signature verification failed"

The node and the image are in different trust domains. A node only installs
images signed by the key that signed the firmware it is currently running, so
a node you flashed with a build from source (signed with your dev key) will
not take official CI-signed images, and vice versa. Crossing back is a USB
flash; there is no over-the-air route, deliberately
([design/ota-signing.md](design/ota-signing.md)).

### "OTA rejected: version `<x>` is below the anti-rollback floor"

The node refuses anything below the highest version it has booted. Tick
**Allow downgrade** on the confirm step in the web client (or pass
`allow_downgrade` over RPC, see [ota-rollout.md](ota-rollout.md)). That also
lowers the floor, so the node is not stranded.

### "Could not load the release index from ..."

The web client could not read the list of published builds. In a browser this
is usually CORS: the release index is fetched from the page, so the update
server has to send an `Access-Control-Allow-Origin` header the page's origin
satisfies. The packaged desktop app fetches it from its main process instead
and is not subject to that. Details, including the current state of the
public update server, are in [ota-rollout.md](ota-rollout.md).

## Simulator and emulator

### Port 3000 is already in use, or the wrong UI loads

Both the simulator and the emulator serve on port **3000** by default when run
locally, so they collide if you start both. Their Docker variants do not:
the simulator's compose file uses 3003 and the emulator's uses 3004.

Override the port instead of stopping one of them:

```bash
# emulator
cd emulator && make run PORT=3010

# simulator (gosim takes the flag directly)
./gosim/bramble-gosim --ui ui/dist --scenarios scenarios --port 3011
```

If you loaded a UI and the scenario list looks wrong, you are very likely
looking at the other one on the shared port.

### `emulator/ci/run_scenarios.sh` reports a missing node binary

```text
SETUP FAIL: node binary missing: emulator/node/build/bramble-node.elf
```

The scenario suite runs the compiled firmware; it does not build it for you.
Build it first:

```bash
cd emulator
make node          # or: make run, which builds everything
bash ci/run_scenarios.sh
```

### A scenario fails intermittently

The emulator scenarios are deterministic by construction. An intermittent
failure is a real bug in the code or in the scenario, not noise. Do not add a
retry loop to absorb it.

## Tests and quality gates

### `make ci-quality-*` exits with an error and no message

Several of these targets guard on a tool being installed and, if it is not,
exit non-zero without explaining. For example, with `shellcheck` absent:

```text
$ make ci-quality-shellcheck
command -v shellcheck >/dev/null
make: *** [Makefile:76: ci-quality-shellcheck] Error 1
```

That is a missing tool, not a failing check. The tools these targets need:

| Target | Needs | Where to get it |
| --- | --- | --- |
| `ci-quality-shellcheck`, `ci-fw-shellcheck` | `shellcheck` | <https://www.shellcheck.net/> or your package manager |
| `ci-quality-cppcheck` | `cppcheck` | <https://cppcheck.sourceforge.io/> or your package manager |
| `ci-quality-actionlint`, `ci-fw-actionlint` | `actionlint` | <https://github.com/rhysd/actionlint> (`ci-fw-actionlint` falls back to `go run` if Go is installed) |
| `ci-quality-ruff` | `uvx` | <https://docs.astral.sh/uv/> |
| `ci-fw-clang-format` | `clang-format` | your LLVM package |
| `ci-quality-board-build` | ESP-IDF v5.4.1 | [BUILDING.md](BUILDING.md) |

You do not need all of them. CI runs the full set; locally, install the ones
covering what you changed, or push and let CI tell you.

### `scripts/lint/run-markdownlint.sh` fails with "neither markdownlint-cli2 nor npx found"

It needs `markdownlint-cli2` on PATH or `npx` (to fetch the pinned version on
demand); there is no skip mode, a missing tool is a hard failure, not
advisory signal (docs/quality-policy.md). Install Node (which provides
`npx`), or install the pinned version globally yourself:
`npm install -g markdownlint-cli2@0.23.1`.

### The host tests fail to build

`bash test/run_all_tests.sh` compiles natively and needs no ESP-IDF, only a C
toolchain and `make`. If it fails to build at all rather than failing a test,
suspect a missing compiler before suspecting the code.

## Commits and CI

### My commit was rejected for an em dash

Em dashes are banned repo-wide, in code, docs, commit messages, and PR bodies.
Replace it with a colon, a comma, or restructure the sentence. The pre-commit
hook checks added lines only, so an em dash already in the file will not block
you; one you add will.

### My commit fails on a missing webapp dependency

The pre-commit hook runs `make check-fast`, which typechecks and unit-tests
the webapp. That needs `webapp/node_modules`, so run `npm ci` in `webapp/`
once even if you are not touching webapp code.

### The hooks never run

They are not installed by cloning. Run `make setup-hooks` once per clone and
confirm:

```bash
$ git config core.hooksPath
githooks
```

### CI rejects my PR title

The title must be a valid conventional commit, `type(scope): subject`, and the
scope must be in the `scope-enum` list in `commitlint.config.cjs`. See
[CONTRIBUTING.md](../CONTRIBUTING.md).

### `check-no-internal-refs.sh` flags my change

The repository is public and this gate blocks internal hostnames, private LAN
addresses, real device addresses, MAC addresses, personal filesystem paths,
and real-world coordinates. Use documentation placeholders instead: RFC 5737
addresses such as `192.0.2.100`, `AA:BB:CC:DD:EE:FF` for MACs, and fictional
coordinates in fixtures. Run it before every push:

```bash
bash scripts/lint/check-no-internal-refs.sh
```
