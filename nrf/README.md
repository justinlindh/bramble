# Bramble nRF52840 target (P0 bring-up)

Experimental port of Bramble to the nRF52840 (Seeed Wio-WM1110 dev kit now,
SenseCAP T1000-E later). Bare-metal FreeRTOS + nrfx, no ESP-IDF, no
SoftDevice; the portable protocol components compile unchanged and the
platform seams are shimmed (`shim/`).

Status, stated per the repo's honesty conventions: builds, boots, and runs
the portable protocol stack (identity, routing tables, DM table, message
store, text UI state machine, crypto) on the Wio-WM1110 dev kit bench, with
the crypto backend pinned to the ESP32 fleet's behavior by shared
standards-vector suites on the host (`test/test_*_nrf_backend`). There is no
radio driver yet (`src/radio_null.c`; the LR1110 backend is phase P1), no
BLE, no RPC transport, and no flash persistence (NVS is RAM-backed for P0;
LittleFS lands in P2). Not a supported device.

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
report and fails the build if RAM demand exceeds the 200KB budget.

Flash layouts: the default `swd` layout links at 0x0 for the dev kit. For
the T1000-E's stock Adafruit UF2 bootloader, configure with
`-DBRAMBLE_NRF_LAYOUT=uf2` to link at 0x26000 and emit `bramble-nrf.uf2`
(family 0xADA52840) for drag-and-drop flashing.

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
| RAM total | 150,464 / 262,144 | 57.4%, 54,336 under the 200KB gate |
| Flash | 43,704 / 1,048,576 | 4.2%, Berkeley text+data (includes .data load image) |

These measurements supersede the scoping spike's 190-230KB estimate. Largest
static objects: `ucHeap` 48K, `s_dm_table` 44K (the DM session table, the
top shrink knob if P2 needs room), `s_msgs_storage` 14K. The libc heap and
MSP stack sizes are untuned nrfx defaults with obvious headroom to reclaim.
Not yet in this image: LR1110 driver + radio buffers (P1), NimBLE (~30K, P2),
and mesh_task statics from `main/` (P2); the 54KB slack is their landing
zone, which is why the gate does not move.

On-target crypto timings (Wio-WM1110 bench, 64MHz, software crypto, small
ECP window): full identity generation 542ms one-time; Ed25519 sign plus two
verifies 103ms.
