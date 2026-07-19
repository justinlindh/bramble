# QEMU backend (phase 2)

True-VM backend for the Bramble emulator: the flashable pager image running in
espressif/qemu's esp32s3 machine. See
`docs/archive/plans/emulator-phase2-qemu-spec.md` for the spec. This
directory holds the dev-loop tooling; device models (GPIO, GPSPI2, SX1262,
SSD1680) arrive in P2.2-P2.5.

## P2.1 spike verdict: GO

The qemu-variant pager image boots through the bootloader, heap init, FreeRTOS
startup, NVS, identity generation (hardware SHA over GDMA), and board_init's
GPIO/SPI-bus setup, and wedges exactly where the spec predicted: the first real
GPSPI2 transaction, `display_flush()` -> `epd_write_cmd(0x12)` ->
`spi_device_polling_transmit()` spinning in `spi_hal_usr_is_done` because the
GPSPI2 peripheral is not modeled. CPU1 parks cleanly in the idle task. That
wedge is P2.3's entry point.

Two findings that were NOT in the spec's expected-wedge list:

1. **QEMU version is load-bearing**: esp-develop-9.0.0 (the version ESP-IDF
   v5.4's `idf_tools.py` installs) crashes the emulator process (double free)
   on the first hardware-SHA-over-GDMA operation, which identity keygen hits.
   Fixed in esp-develop-9.2.2. **Pin esp-develop-9.2.2-20260417.**
2. **ADC self-calibration wedges before app_main**: the battery monitor links
   `esp_adc`, whose calibration runs in a global constructor. On a blank eFuse
   it busy-waits on the unmodeled SAR ADC before the scheduler starts. Fixed
   with data, not code: an eFuse image with `BLK_VERSION_MAJOR=1` (ADC calib
   V1) routes calibration through eFuse reads. `mkefuse.py` generates it;
   `run-qemu.sh` wires it in.

## Getting QEMU

Prebuilt (recommended):

```sh
curl -fLO https://github.com/espressif/qemu/releases/download/esp-develop-9.2.2-20260417/qemu-xtensa-softmmu-esp_develop_9.2.2_20260417-x86_64-linux-gnu.tar.xz
tar -xJf qemu-xtensa-softmmu-esp_develop_9.2.2_20260417-x86_64-linux-gnu.tar.xz
export QEMU_XTENSA=$PWD/qemu/bin/qemu-system-xtensa
```

The binary needs `libslirp.so.0`. If the host does not have it, install the
distro package (`libslirp`), or extract the library anywhere and set
`LD_LIBRARY_PATH`.

Do NOT use `idf.py qemu` / `idf_tools.py install qemu-xtensa` with IDF v5.4:
it installs 9.0.0, which aborts on SHA-over-GDMA (see above).

## Build QEMU from source

The prebuilt binary is fine for a stock boot, but it cannot run our device
models (GPIO, GPSPI2, SX1262, SSD1680): QEMU has no stable plugin API for
adding devices, so P2.2-P2.5 build them straight into the QEMU tree.
`bootstrap-qemu.sh` stands up that pipeline:

```sh
emulator/qemu/bootstrap-qemu.sh
```

It clones `espressif/qemu` at the pinned tag into `$QEMU_SRC` (default
`~/src/qemu-esp`), injects the model sources from `emulator/qemu/models/`
into the tree, applies the wiring patches from `emulator/qemu/patches/`,
configures (`--target-list=xtensa-softmmu --disable-slirp --disable-werror
--disable-gnutls`), builds with ninja, and prints the resulting
`qemu-system-xtensa` path.

`--disable-slirp`: this host has no libslirp pkg-config module and we use
chardev sockets, not user-mode networking; patch
`0002-fix-disable-slirp-not-found-propagation.patch` fixes a bug on this
tag where `--disable-slirp` alone does not actually keep `net/slirp.c` out
of the build (see `patches/README.md`). `--disable-gnutls`: on this tag,
`libgcrypt` (needed for the esp32s3 AES/RSA/DS/XTS_AES device models,
unconditionally instantiated by the machine) is only probed when gnutls's
own crypto backend is not found; this host has gnutls, so without this flag
gcrypt is never probed even though it is present, and QEMU aborts at
machine realize with `unknown type 'misc.esp32s3.aes'`. We do not need
gnutls (no TLS use in this setup). It is idempotent: re-running it reuses
the existing clone, skips patches already applied, and lets ninja no-op.

`emulator/qemu/models/` holds the device model C/H sources; `.c` files land
in the QEMU tree's `hw/xtensa/` (next to `esp32s3.c`) and `.h` files land in
`include/hw/xtensa/`, mirroring where the esp32s3 machine's own files live.
`emulator/qemu/patches/` wires those files into `hw/xtensa/meson.build` and
into `esp32s3_machine_init()`. See both directories' READMEs for details.
Neither `$QEMU_SRC` nor its `build/` directory is part of this repo.

`run-qemu.sh` picks up the from-source build automatically: if `QEMU_XTENSA`
is unset, it defaults to `$QEMU_SRC/build/qemu-system-xtensa` when that file
exists, falling back to `PATH` otherwise (the prebuilt-binary workflow above
still works unchanged).

## Build the image

From the repo root, with the IDF v5.4 environment exported:

```sh
SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.bramble_pager;sdkconfig.defaults.qemu" \
  idf.py -B build-qemu -DSDKCONFIG=build-qemu/sdkconfig set-target esp32s3 build
```

`sdkconfig.defaults.qemu` relaxes exactly what QEMU cannot do and nothing
else: app signing off (QEMU S3 has no Secure Boot), flash encryption pinned
off, console on UART0 (no USB Serial/JTAG model). Same code paths, same
partition table, same binary layout otherwise. `-DSDKCONFIG=build-qemu/sdkconfig`
keeps the variant's sdkconfig out of the repo root so it cannot clobber a
hardware build's config.

## Run

```sh
emulator/qemu/run-qemu.sh            # UART0 on stdio
emulator/qemu/run-qemu.sh --fresh    # discard prior flash state (NVS identity)
```

The script merges the 8MB flash image (esptool merge_bin, once per build),
generates the eFuse image, and boots. QEMU writes flash back to
`build-qemu/flash_qemu.bin`, so the node keeps its identity across runs;
`--fresh` gives it a new one. Exit QEMU with `Ctrl-A x`.

Expected output today: full boot to `BOOT STAGE: show_splash`, then silence
(the GPSPI2 spin). `W (xxx) rtcinit: o_code calibration fail` early in boot is
a known harmless QEMU artifact, as is the `SLOW READ` SPI boot mode (flash
mode eFuse bits are zero in the generated image).

## Debug (gdb)

```sh
emulator/qemu/run-qemu.sh --gdb      # freeze at reset, gdbserver on :1234
```

then in another terminal:

```sh
xtensa-esp32s3-elf-gdb build-qemu/bramble.elf \
  -ex "target remote :1234" -ex continue
```

`Ctrl-C` in gdb stops the target anytime; `info threads` shows both cores,
`bt` symbolizes fully against `bramble.elf`. Attaching gdb to an already
running QEMU started with `-s` (no `-S`) also works and stops the target on
attach, which is the fastest way to ask "where is it stuck".

## What the QEMU S3 machine gave us for free

Verified working in the spike: NOR flash + MMU + partition access, file-backed
eFuse, RNG entropy, dual-core FreeRTOS scheduling, systimer/esp_timer, GDMA,
hardware SHA (on 9.2.2), NVS reads and writes, UART0 console. Not modeled and
therefore this phase's work: GPSPI2/3, GPIO matrix beyond boot straps, SAR
ADC, USB Serial/JTAG.
