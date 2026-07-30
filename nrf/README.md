# Bramble nRF52840 target

Experimental port of Bramble to the nRF52840: the Seeed Wio-WM1110 dev kit
and the SenseCAP T1000-E card tracker. Bare-metal FreeRTOS + nrfx, no
ESP-IDF, no SoftDevice; the portable protocol components compile unchanged
and the platform seams are shimmed (`shim/`).

The port landed in phases, and the measured tables below keep their phase
labels: P0 protocol core on the dev kit, P1 LoRa radio, P2 BLE RPC + flash
persistence, P3 (not yet started) GNSS and power management.

Status, stated per the repo's honesty conventions: builds, boots, and runs
as a live peer on the bench mesh from the Wio-WM1110 dev kit, with the
fleet's BLE GATT server, the full RPC surface, and flash persistence (P2).
The LR1110 radio backend (`src/radio_lr1110.c`, Semtech SWDR001) transmits
and receives; the real `main/` mesh loop runs (beacons, neighbor discovery,
attestation relay, forwarding); NimBLE (host and controller, no SoftDevice)
runs alongside it, pairing with LE Secure Connections and persisting bonds;
identity, network key, messages and bonds live in LittleFS and survive
reboot; the crypto backend is pinned to the ESP32 fleet by shared
standards-vector suites on the host (`test/test_*_nrf_backend`).

P2 exit gate, bench-verified 2026-07-28 on the 7-node bench mesh: an ERASED
device (fresh identity, no network key, node INERT) was provisioned entirely
over an encrypted BLE link exactly as the webapp does it: paired (Just
Works over LE Secure Connections), authenticated with the per-device token,
`bramble.setNetworkKey` over the link, after which it joined the mesh (7
named neighbors), its broadcast was received and stored by a bench T-Deck,
and after a reset the SAME identity, network key and BLE bond all loaded
from flash and the node rejoined inside a minute. A 10-minute soak followed
with continuous heartbeats and a stable heap. BLE pairing reliability: 45
consecutive connected attempts reached encryption with zero link kills
(the controller previously terminated ~20% of attempts; see
`patches/nimble-dup-pdu-during-enc-start.patch`).

T1000-E, verified on the physical card 2026-07-28: flashes through its
stock Adafruit UF2 bootloader, boots, advertises over BLE under the
address-derived default name, completes phone RPC sessions, exchanges LoRa
mesh traffic with the ESP32 fleet, and reflashes remotely over BLE
(`bramble.enterDfu` + `scripts/flash_ble.py`). The board is consoleless, so
boot failures are diagnosed through the flash boot trace (below) instead of
a UART.

Still absent: GNSS (P3), power management (P3, the 32MHz crystal stays on,
so battery life is untuned). Not a supported device yet.

## Build

Needs `arm-none-eabi-gcc`, CMake >= 3.24, Ninja, Python 3. Dependencies
(nrfx, CMSIS, FreeRTOS-Kernel, mbedtls, Monocypher, cJSON) are
FetchContent-pinned in `nrf/CMakeLists.txt` and
`components/crypto/crypto_deps.cmake` (the crypto pins are shared with the
host test build so both compile the exact same library versions and mbedtls
config), cached in `build/_deps`.

```sh
cmake -S nrf -B nrf/build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake -DCMAKE_BUILD_TYPE=MinSizeRel
cmake --build nrf/build
```

Every build ends with `scripts/size_report.py`, which prints the memory
report and fails the build on any of three limits: total RAM over
252KB, static (non-heap) RAM over 104KB, or the heap below its 144KB floor.

Board selection: `-DBRAMBLE_NRF_BOARD=wm1110_devkit` (default) or
`t1000e`. The board header (`boards/`) owns the pin map, the LR1110 RF
switch truth table, and whether a console UART exists at all; the T1000-E
has no USB-UART bridge, so it builds consoleless and BLE is the only
interface.

Flash layouts: the default `swd` layout links at 0x0 for the dev kit. The
T1000-E build configures with `-DBRAMBLE_NRF_LAYOUT=uf2
-DBRAMBLE_NRF_BOARD=t1000e` to link at 0x27000 (the app base under its
stock bootloader's S140 v7.3.0 layout, confirmed on hardware) and emit
`bramble-nrf.uf2`. The UF2 is padded to whole 256-byte blocks because the
stock bootloader silently rejects a short final block and then never
completes the download; see `scripts/make_uf2.py` for both that and why the
family ID stays the generic nRF52840 one.

## Flash and debug (dev kit)

Probe: any CMSIS-DAP adapter on the board's SWD header (GND/3V3/CLK/DIO,
right of the module). pyOCD drives it; on Linux the probe needs a udev rule
granting user access to VID:PID 0d28:0204 (`TAG+="uaccess"`), applied on a
fresh plug-in.

```sh
uv tool install pyocd            # once
pyocd flash -t nrf52840 nrf/build/bramble-nrf.hex
pyocd cmd -t nrf52840 -c reset
```

Console: the dev board's USB-C exposes a CH340 serial bridge wired to the
nRF's UART1 (TX P0.24 / RX P0.22, determined on the bench; see
`boards/wio_wm1110_devkit.h`). 115200 8N1:

```sh
stty -F /dev/ttyUSB0 115200 raw -echo && cat /dev/ttyUSB0
```

Note: logging uses newlib-nano printf, which has no `%lld`; keep 32-bit
format specifiers in target logs.

## Flash (T1000-E, UF2 bootloader)

Two paths into the stock bootloader's UF2 volume:

- First flash (device still running vendor firmware, or Bramble is dead):
  the manual gesture from Seeed's user guide: hold the button and quickly
  connect the USB cable twice; the green LED goes solid and a `T1000-E`
  drive appears. Copy `bramble-nrf.uf2` onto it. Timing-sensitive, expect a
  few attempts.
- Every flash after that: `bramble.enterDfu` over BLE reboots the running
  firmware straight into the bootloader. `scripts/flash_ble.py` drives the
  whole loop (authenticate, request DFU, wait for the volume, copy, confirm
  the app boots):

```sh
uv run --with bleak python nrf/scripts/flash_ble.py <BLE name> \
  nrf/build-t1000e/bramble-nrf.uf2 --token <device RPC token>
```

The bootloader flashes the image but never resets by itself; the firmware's
first job on this board is claiming VTOR and quiescing the NVIC, because the
bootloader enters the app by a jump, not a reset (see `src/main_nrf.c`).

## Boot trace (consoleless diagnostics)

Consoleless boards record boot progress as (tag, aux) word pairs in a
reserved flash page (`src/boot_trace.h`, page 0xBF000). Every fatal path
(assert, hard fault, stack overflow, heap exhaustion, and a two-minute
no-advertising sentinel on UF2 builds) stamps the trace and reboots into the
bootloader instead of halting, because a halted consoleless board is
indistinguishable from a dead one. From the bootloader, the trace reads back
through the `CURRENT.UF2` flash dump with no debugger attached:

```sh
python3 nrf/scripts/read_boot_trace.py /run/media/$USER/T1000-E/CURRENT.UF2
```

## Dev network key (bench only)

Bench builds may seed the network key at configure time:
`cmake ... -DBRAMBLE_NRF_DEV_NETKEY=<64 hex>` (the emulator's
`EMU_NETWORK_KEY` pattern, compile-time edition). Consoleless boards have
the same problem with the RPC auth token (it is minted once and logged to a
UART that does not exist), so `-DBRAMBLE_NRF_DEV_AUTH_TOKEN=<token>` seeds
one at first boot when none is stored. Neither value is ever committed;
without the netkey define the node boots INERT exactly like an
unprovisioned fleet node.

## Measured memory (P0 exit gate)

Measured 2026-07-27 on the full P0 image (portable protocol stack + crypto +
FreeRTOS + null radio), via `scripts/size_report.py` (runs on every build
and fails it over budget):

| Item | Bytes | Notes |
|---|---|---|
| .bss | 117,580 | includes 48KB FreeRTOS heap (`ucHeap`) |
| .data | 112 | |
| libc heap (.heap) | 16,388 | nrfx startup default, newlib only |
| MSP stack | 16,384 | nrfx startup default |
| RAM total | 150,464 / 262,144 | 57.4% (figure predates the P2 heap unification) |
| Flash | 43,704 / 1,048,576 | 4.2%, Berkeley text+data (includes .data load image) |

These measurements supersede the scoping spike's 190-230KB estimate. Largest
static objects: `ucHeap` 48K, `s_dm_table` 44K (the DM session table, the
top shrink knob if P2 needs room), `s_msgs_storage` 14K. The libc heap and
MSP stack sizes are untuned nrfx defaults with obvious headroom to reclaim.
Not in the P0 image measured below: LR1110 driver + radio buffers (P1), NimBLE (P2),
and mesh_task statics from `main/` (P2); the 54KB slack is their landing
zone, which is why the gate does not move.

On-target crypto timings (Wio-WM1110 bench, 64MHz, software crypto, small
ECP window): full identity generation 542ms one-time; Ed25519 sign plus two
verifies 103ms.

## Measured (P1, full mesh image)

Measured 2026-07-27 on the P1 exit-gate image (LR1110 radio + full mesh
loop): RAM 195,936 / 262,144 bytes (74.7%; figure predates the P2 heap
unification, which moved plain malloc into ucHeap and made the total honest) with
the FreeRTOS heap at 96KB and the MSP stack/libc heap tuned to 8KB/4KB;
flash 151,912 bytes (14.5%). On-air: first beacon TX 230 bytes; 10 beacons
per 10-minute soak at the adaptive interval; LR1110 TCXO runs at 1.6V/164
ticks (the vendor SDK's 3.0V was not needed; no calibration errors).
