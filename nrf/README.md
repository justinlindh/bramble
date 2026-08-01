# Bramble nRF52840 target

Experimental port of Bramble to the nRF52840: the Seeed Wio-WM1110 dev kit
and the SenseCAP T1000-E card tracker. Bare-metal FreeRTOS + nrfx, no
ESP-IDF, no SoftDevice; the portable protocol components compile unchanged
and the platform seams are shimmed (`shim/`).

The port landed in phases, and the measured tables below keep their phase
labels: P0 protocol core on the dev kit, P1 LoRa radio, P2 BLE RPC + flash
persistence, P3 GNSS (below). Power management has since landed too: WFE
idle-sleep in the idle task, corrected HFXO ownership (BLE releases the
crystal instead of holding it across every idle period), and real battery
telemetry on the T1000-E (SAADC + charge/VBUS detect,
`nrf/shim/battery_saadc.c`). What remains is bench verification against
real hardware (overnight drain bound, current-draw measurement), not
implementation.

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

GNSS (P3) is implemented for the AG3335 module (see below) and bench-verified
on the physical T1000-E, 2026-07-31 (see the GNSS section for what was
checked).

Power management, corrected from an earlier version of this README that
claimed the 32MHz crystal (HFXO) stays on continuously: it does not, and
never did on this port. NimBLE's link-layer rfmgmt (upstream, unmodified)
already duty-cycles HFXO off between BLE events, requesting it 1500us
ahead of each radio event, through direct CLOCK-peripheral register writes
(source-verified: `nimble/controller/src/ble_ll_rfmgmt.c`; binary-verified
by disassembling the shipped ELF, where `ble_phy_rfclk_enable/disable` are
raw register stores, not calls into this project's own glue code in
`src/nimble_glue.c`, which is vestigial on this FreeRTOS build). What this
project's own power work has changed: the CPU's idle task now sleeps via
WFE instead of busy-spinning at 64MHz (binary-verified against the fetched
FreeRTOS kernel source), the boot path now stops HFXO once LFCLK bring-up
finishes instead of leaving it on until BLE's first natural duty cycle,
and nRF52840 anomaly 192 (LFRC calibration frequency error) is now worked
around in the RC-oscillator fallback path (both source-verified against
Nordic's documented errata and nrfx's own reference implementation, not
yet bench-tested since every board on the bench fleet has a 32.768kHz
crystal fitted, so that fallback path never runs). Bench-pending: overall
current draw under real BLE activity and idle, and outdoor cold-start
TTFF, which the GNSS bench pass did not measure. Not a supported device
yet.

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

## GNSS (T1000-E)

The T1000-E carries an Airoha AG3335 GNSS module wired to the nRF's UARTE1
(the board has no console UART, see above). The driver
(`shim/gps_t1000e.c`) satisfies the same `components/gps/include/gps.h`
contract as the ESP32 fleet and the emulator's virtual GPS: an nrfx event
handler double-buffers EasyDMA bytes into a 512B FreeRTOS stream buffer, a
dedicated "gnss" task drains that buffer into `components/gps/gps_feed.c`
(the platform-free NMEA parser shared by every backend), and the feed's fix
callback emits the same `bramble.onGpsEvent` shape regardless of which
fleet the node runs on. Every power transition (the initial power-on and
every later toggle) also runs on that task, never on the caller, so neither
the boot path nor the RPC handler behind `bramble.setGpsEnabled` ever blocks
on the module's power-on sequence.

Pin map: `boards/t1000e.h`, sourced from Meshtastic's `tracker-t1000-e`
board variant, the field-proven community map for this hardware (EN,
RESET, VRTC_EN, SLEEP_INT, RTC_INT, RESETB, plus TX/RX on UARTE1 at
115200 8N1).

Power-on sequence: EN high, a reset pulse, an RTC_INT wake pulse followed
by about a second of `$PAIR382` wake commands (Meshtastic treats this as
required on this module), then a `$PAIR0xx` configuration burst that
enables GPS, GLONASS, Galileo, and BeiDou and turns on GGA, RMC, and GSV
sentences (GLL, GSA, VTG, and ZDA stay off), saved to the module's flash
with `$PAIR513`. GSV is a deliberate deviation from Meshtastic's variant:
it is the only sentence carrying satellites-in-view, which the diagnostics
below expose for a "searching" UI before the first fix. The whole sequence
blocks the gnss task for close to two seconds while the module is already
talking; VRTC_EN is driven high once at boot and never cleared, so the
module keeps backup power across `gps_set_enabled()` toggles and gets a
warm start on the next power-on.

Enable preference and RPC toggle: `components/gps/gps_pref.c` persists a
boolean (`bramble/gps_en` NVS key, default on). `bramble.setGpsEnabled`
flips it and applies it live through `gps_set_enabled()`;
`bramble.getStatus` reports the current value as `gps_enabled`, independent
of the `gps_available` capability flag, and the webapp's Settings screen
exposes it as a toggle.

Duty-cycling: `components/gps/gps_duty.c` is a pure, host-tested policy
(`test/test_gps_duty.c`) that the gnss task evaluates once a second against
the mesh's location-share state. GNSS stays off outright if the user
preference is off; otherwise it stays powered continuously unless the node
is actively sharing location on an interval of at least
`GPS_DUTY_MIN_INTERVAL_S` (120s; below that, cycling saves nothing against
the margin cost). A node that has never sent a share yet also keeps GNSS
powered continuously, since there is no prior send time to schedule a wake
against; the cycling behavior only starts after the first share goes out.
From there, it powers down between fixes and wakes
`GPS_DUTY_WARM_MARGIN_S` before the next scheduled send to leave time for a
warm reacquisition. That margin defaults to 60s. The bench measured warm
TTFF at 1 to 2 seconds with VRTC held under open sky, but 60s is a
deliberate judgment on top of that measurement, not a scaled-up version of
it: it covers a warm start after ephemeris has gone stale (a multi-hour
park), which re-downloads ephemeris and takes tens of seconds, a case the
short bench soak did not exercise. See `components/gps/include/gps_duty.h`
for the full reasoning.

Diagnostics: `bramble.getDiagnostics` reports `gps_rx_bytes` and
`gps_rx_lines` (total bytes and NMEA-ish lines seen since the driver last
started), `gps_chip` (the first `$PAIR021*` chip-identification banner
line, empty if none seen), and `gps_rx_overruns` / `gps_rx_errors` (bytes
dropped when the stream buffer was full, and UARTE error events), present
only on GPS-capable boards. Zero rx bytes with the driver running means the
UART link is dead; nonzero bytes with zero lines means data is flowing but
not parsing.

Verification status: the shared NMEA feed (`components/gps/gps_feed.c`)
and the duty-cycling policy (`components/gps/gps_duty.c`) are host-tested
via `bash test/run_all_tests.sh`. The T1000-E driver itself
(`shim/gps_t1000e.c`) was bench-verified on the physical card, 2026-07-31:
NMEA flowed from the AG3335 with zero stream-buffer overruns, and the chip
banner identified the module as an AG3335M running firmware V2.6.0. Cold
time-to-first-fix measured about 22 minutes indoors (outdoor cold TTFF was
not measured this pass). Warm TTFF measured 1 to 2 seconds with VRTC held,
under open sky. `bramble.shareLocationOnce` delivered a live GPS position
to a bench peer over the mesh. `bramble.setGpsEnabled` and the underlying
preference were verified to toggle GNSS power live and to persist across a
reboot. Duty-cycling was observed powering the module down after a share
went out and waking it again at `last_send + interval - GPS_DUTY_WARM_MARGIN_S`,
matching the policy. A 45-minute soak ran with a byte-stable heap, mesh RX
observed every minute, and zero stream-buffer overruns throughout.

Still bench-pending: overall current draw under real duty cycling (see the
power-management note above for what has landed source- and
binary-verified versus what still needs a bench), outdoor cold-start TTFF,
and reacquisition after a multi-hour park where ephemeris has gone stale.
Airoha's published AG3335 chip
specification states a cold-start time to first fix under 25 seconds and a
tracking sensitivity of -167 dBm; Airoha does not publish current
consumption figures for the chip, so none are cited here.

## Battery (T1000-E)

The T1000-E carries a LiPo cell plus charger-IC status pins (Seeed's stock
Meshtastic-compatible wiring). `shim/battery_saadc.c` reads the cell voltage
over P0.02/AIN0 through the board's 2x divider using the nRF52840's SAADC,
and reads charge/VBUS-detect (P1.03, P0.05) as plain GPIO inputs, satisfying
the same `components/battery/include/battery.h` contract as the ESP32 fleet
and the emulator's virtual battery: `battery_get_status()` returns averaged
millivolts, a curve percentage, presence, and a hardware-informed charging
state. Every field this backend produces flows through the shared code the
ESP32 fleet already uses: the same LiPo discharge curve
(`components/battery/battery_pct.c`), the same beacon-level 0xFF
unknown/plugged-in sentinel (`battery_beacon_pct()`,
`docs/bramble-protocol-spec.md` §beacon layout), the same RPC charging
fields on `getStatus`/`getBattery`, and the same webapp charging display.
The pin wiring and charge-detect polarity are source-verified against
Meshtastic's `tracker-t1000-e` variant and `Power.cpp` (cited in
`battery_saadc.c`'s header comment), not against this project's own bench
measurement of the T1000-E's battery circuit.

Verification status: source-level only. The SAADC driver builds and its
pure logic (averaging, curve mapping, charging classification) is
host-tested the same way the ESP32 path is, but the actual voltage and
charge-detect readings have not yet been checked against real hardware
(a multimeter, a live charge/discharge cycle, or a charger being plugged
and unplugged on the physical card): that bench pass is still pending. The
Wio-WM1110 dev kit has no battery hardware and never claims otherwise:
`shim/battery_null.c` reports `present=false` honestly rather than
fabricating a reading.

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

Neither table above includes the T1000-E's GNSS driver (see the GNSS
section above): both were measured on the Wio-WM1110 dev kit build, which
has no GPS hardware and compiles it out. A T1000-E image size, including
the GNSS driver, is a separate figure from the GNSS functional bench pass
above and has not been measured yet.
