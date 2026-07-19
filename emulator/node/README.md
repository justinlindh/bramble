# bramble-node: the firmware on the IDF linux target

This project builds the real Bramble firmware (`main/` + `components/`,
unmodified sources) for ESP-IDF's experimental POSIX/Linux simulator and runs
it as a host process. It exists as the runtime for the emulator's virtual
pagers (see `emulator/DESIGN.md`); in its current spike form the peripherals
are no-op stubs (`null_drivers/`).

## Build and run

```
source $IDF_PATH/export.sh          # ESP-IDF 5.4.1
cd emulator/node
idf.py --preview set-target linux
idf.py build
./build/bramble-node.elf
```

`./spike_check.sh` boots the node for 15 seconds and asserts the gate
criteria: app_main reaches mesh_task, the null radio initializes, the beacon
path is entered (INERT, no TX: node unprovisioned), and the process idles
(< 20% CPU; measured 0.0-0.2%).

AddressSanitizer build (separate build dir):

```
idf.py -B build-asan build -DCMAKE_C_FLAGS=-fsanitize=address \
    -DCMAKE_CXX_FLAGS=-fsanitize=address -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address
./spike_check.sh build-asan/bramble-node.elf
```

## GATE VERDICT (Task 1): GO

The IDF linux target is viable as the emulator's node runtime. The full
firmware (mesh_task.c as-is, real crypto via mbedtls + libsodium, real NVS,
real tx_gate) boots to the main mesh loop, enters the beacon path (INERT,
no TX: node unprovisioned), and idles at ~0% CPU. A 45 s AddressSanitizer
run is clean. No mesh_task logic was stubbed or forked; the only firmware
source changes were gates at existing hardware seams plus three genuine
portability fixes (listed below). Proceed with Tasks 2+ as planned (no
FreeRTOS-shim fallback needed).

### The one surprise the plan must absorb

`ESP_PLATFORM` is defined for EVERY idf.py build, including the linux
target (`tools/cmake/build.cmake`). The repo's `#ifdef ESP_PLATFORM`
convention therefore does not distinguish device from simulator; the linux
target is a third environment where both `ESP_PLATFORM` and
`CONFIG_IDF_TARGET_LINUX` are defined. Consequences:

- Files whose device half is pure IDF-API code (identity, channel_storage,
  network_key, msg_store, crypto: all NVS/mbedtls/libsodium based) compile
  their REAL device half on the simulator. This is a feature: more real code
  runs than the plan assumed.
- Files whose device half touches drivers gate the driver-dependent parts
  with `#if defined(ESP_PLATFORM) && !defined(CONFIG_IDF_TARGET_LINUX)`
  (board_config.h, board.c, gps.c). `CONFIG_IDF_TARGET_LINUX` is IDF's own
  documented macro for this, not a custom scheme, and the plain-gcc test
  harness (no ESP_PLATFORM, no sdkconfig) is unaffected.
- main/-only files (never built by the gcc harness) use plain
  `#ifndef CONFIG_IDF_TARGET_LINUX` gates.

### Caveats hit (IDF-linux gaps), and how they are handled

1. **Requiring a non-linux component hard-fails configure.** `driver`,
   `esp_wifi`, `bt`, `esp_adc`, `esp_vfs_console`, `app_update`,
   `esp_https_ota`, `sdmmc`, `esp_driver_spi/gpio/i2s`, `bootloader_support`
   all `return()` without registering on linux. Every bramble component that
   names them now has an `if(target STREQUAL "linux")` branch in its
   CMakeLists (usually headers-only registration; the runtime symbols come
   from `null_drivers/`).
2. **esp_timer is header-only on linux** (support matrix says mock-only).
   `esp_timer_posix.c` implements the used surface (get_time, create,
   start_once/periodic, stop, delete) on FreeRTOS software timers plus a
   monotonic microsecond clock. Same for **esp_task_wdt** (no-ops; there is
   no hardware to feed and spike_check.sh watches CPU instead).
3. **mdns 1.11.3 does not build on linux** (wants `esp_netif_linux` from its
   own test rig). Excluded via a manifest rule in `main/idf_component.yml`;
   call sites gated (only reachable from the WiFi-connected path anyway).
4. **Single simulated core.** `xTaskCreatePinnedToCore(..., 1)` asserts;
   mesh_task uses `tskNO_AFFINITY` on the simulator.
5. **UART/USB console does not exist**, so `cli.c` is excluded on linux.
   Follow-up: IDF's esp_console has a linux REPL backend
   (`esp_console_repl_linux.c`) if a CLI is ever wanted here; RPC via the
   emu-link is the plan of record.
6. **NVS is file-backed but ephemeral by default**: esp_partition emulates
   flash in a fresh temp file per process, so identity regenerates each run.
   The hook for per-node persistence is `esp_partition_file_mmap_ctrl_input()`
   (set a flash file path before NVS init); wiring it to a per-node dir is
   Task 2/5 plumbing (gosim node supervisor).
7. **heap_caps sizes report UINT32_MAX** on the host (no heap_caps
   implementation); the periodic heap diagnostics log line is cosmetic noise.
8. **WiFi/BLE**: BLE uses the existing `ble_server_stub.c` (NimBLE can never
   be enabled on linux). esp_wifi does not exist, so `components/wifi` is
   headers-only there and `null_drivers/periph_null.c` answers "no wifi";
   boot logs "WiFi init failed" and continues, same as a device with no AP.
   ws_server.c itself compiles fine (esp_http_server supports linux), it is
   just never started without WiFi; a direct host-socket bind is the
   DESIGN.md follow-up, out of spike scope.
9. **OTA**: esp_https_ota/app_update are device-only; the pure sources
   (ota_url/ota_version/ota_origin) build and the device-only entry points
   are "not supported" stubs in null_drivers.

### Genuine firmware fixes found by the spike (kept for device builds too)

- `mesh_task.c`: `xPortGetCoreID()` logged with `%d` while `BaseType_t` is
  `long` on the POSIX port (cast added).
- `main.c` / `mesh_task.c`: two snprintf buffers (`b[8]`, `key[8]`) too
  small for a theoretical full-int render; caught by -Wformat-truncation
  under the host toolchain, sized to 16.

### What is NOT exercised yet

- The node is unprovisioned (no network key), so it boots INERT: the beacon
  path runs and logs but intentionally emits nothing. Actual TX through
  tx_gate -> radio_transmit_raw needs provisioning, which needs a transport
  (emu-link RPC, later tasks).
- The RX path (rx callback never fires by design of the null radio).

### Spike scaffolding to be replaced (Tasks 4/5)

`radio_null.c`, `display_null.c`, `periph_null.c` (button/battery/wifi/ota
stubs) and `esp_timer_posix.c`, all wired through the `null_drivers`
component. The real emu_link virtual drivers replace radio/display/button/
battery; esp_timer_posix likely stays (it is IDF-gap glue, not a fake).

### Regression status at spike completion

- 103/103 host test suites pass (`bash test/run_all_tests.sh`).
- Device builds green for heltec-v3 (default) and tdeck-plus sdkconfigs
  (esp32s3).
