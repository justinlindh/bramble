---
name: bench-flash
description: Use when flashing Bramble firmware to physical bench hardware (heltec V3/V4, T-Deck, pager) or when a node boot-loops, loses identity, or stops meshing after a flash. Encodes the encrypted-V3 brick rule, the artifact/identity verification steps, and the regressions only real hardware catches.
---

# Flashing the Bramble bench safely

Real-hardware flashing is the ONLY gate that catches a whole class of bugs.
Host tests, board builds, the emulator and code review were ALL green while the
firmware boot-looped every node. Do not skip it before merging firmware.

## 1. The brick rule (read before touching anything)

The bench **heltec V3 (address `AB246C7C`, CP2102, `/dev/ttyUSB*`) has a BURNED
flash-encryption eFuse.** A plaintext flash bricks it AND destroys its NVS
identity. It must be flashed app-only, encrypted:

```
esptool --chip esp32s3 --port /dev/ttyUSB0 -b 460800 \
  --before default_reset --after hard_reset \
  write_flash --encrypt 0x10000 build-heltec-v3/bramble.bin
```
Never write its bootloader/partition table. `bash scripts/flash-fleet.sh` applies
this rule automatically by ADDRESS and is the safe default for the whole bench.

**Identify nodes by ADDRESS (`bramble.getStatus`), never by port** - ports
renumber on replug, and native-USB (ttyACM) ports re-enumerate on every reset.

## 2. Board names take HYPHENS

`heltec-v3 heltec-v4 tdeck-plus bramble-pager`. An underscore (`heltec_v4`) used
to fall through every check and silently leave BOTH dangerous defaults standing
(BOARD=heltec-v3, the encrypted node; ACTION=flash), i.e. "plaintext-flash the
encrypted V3". `flash.sh` now hard-rejects unknown args, but type them correctly.

## 3. Config changes need the cached sdkconfig deleted

`sdkconfig.<board>` is generated and gitignored. ESP-IDF only applies
`sdkconfig.defaults*` when generating it fresh, so a changed default is SILENTLY
IGNORED on an existing build. Always:
```
rm -f sdkconfig.tdeck-plus && bash scripts/flash.sh local tdeck-plus build
grep CONFIG_<YOUR_KEY> build-tdeck-plus/config/sdkconfig.cmake   # prove it took
```

## 4. Verify the artifact BEFORE flashing

Asset/build staleness is silent. Grep the built binary for a string unique to
your change:
```
strings build-heltec-v4/bramble.bin | grep -q "my new UI string" || echo STALE
```

## 5. Verify AFTER flashing (all three)

- **Booted?** Capture the boot log. On CP2102 (V3) you can reset via DTR/RTS and
  read the console. On native USB (ttyACM) the port re-enumerates, but opening it
  does NOT reset the chip, so you can passively watch a running/looping node.
  Look for the last `BOOT STAGE:` line before any panic.
- **Identity survived?** `bramble.getStatus` address must be UNCHANGED (especially
  the V3 after an encrypted flash). A new random address = NVS was wiped.
- **Meshing?** `beacon_rx` / `packets_rx` must CLIMB and `getNeighbors` must list
  peers. Give it ~2 minutes UNDISTURBED: every probe on a CP2102 port reboots the
  node, so polling in a tight loop prevents the mesh from ever forming.

## 6. Bricked-vs-crashing triage

`esptool --before default_reset read-mac` talks to the ROM, independent of app
firmware. If it answers, the chip is ALIVE and you have a firmware bug, not a
brick. Then read the boot log for the panic.

## Regressions only real hardware catches (all shipped green in CI first)

- **Main-task stack overflow.** On non-LVGL boards the main task IS the UI task;
  a stack-heavy render path (mbedtls HKDF) overflowed the 3584-byte IDF default.
  Boot loop at `display_init`. Fix: `CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192`.
- **Internal-DRAM exhaustion.** The ESP32-S3's INTERNAL DRAM is the scarce
  resource; the "2 MB free heap" a node reports is PSRAM. An ~81 KB static table
  starved internal RAM to ~5 KB, which storms mdns and KILLS RADIO RX (mesh never
  forms, `beacon_rx=0`). Check `getDiagnostics` -> `heap.internal_min_ever_free`.
  Large static tables belong in PSRAM (`heap_caps_calloc(MALLOC_CAP_SPIRAM)`).
- **PSRAM is NOT on every board.** Only heltec_v4 and tdeck_plus have it
  (`CONFIG_SPIRAM`). heltec_v3 and bramble-pager do NOT: `MALLOC_CAP_SPIRAM`
  returns NULL there. NEVER `assert()` on a PSRAM alloc (it also compiles out in
  release). Use the codebase pattern: PSRAM -> internal fallback + warn ->
  ESP_LOGE + fail.

The emulator and host tests have flat, huge RAM and see none of the above.
