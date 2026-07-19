# Bramble device models (QEMU C sources)

Source files here get injected into the espressif/qemu tree by
`bootstrap-qemu.sh`: every `*.c` and this directory's `meson.build` land in
`hw/xtensa/bramble/` (a subdir this project owns, reached by the single
`subdir('bramble')` line `patches/0001-bramble-subdir-meson.patch` adds), and
every `*.h` lands in `include/hw/xtensa/` (next to the esp32s3 machine's own
sibling headers, so the models include them as `hw/xtensa/bramble_*.h`).

Adding a new model is a self-contained edit here - drop the `.c`/`.h`, add the
source to `meson.build`, and add its attach call in `bramble_attach.c` - with NO
new patch to stock QEMU. `meson.build` (the bramble source list) and
`bramble_attach.c` (the machine-init fan-out) are the two places a model wires
in; `patches/` only carries the two one-time hooks that reach them plus two
stock-QEMU fixes (see `patches/README.md`).

- `bramble_scaffold.c` / `.h`: the injection-round-trip proof (a log line at
  machine realize) plus the shared `bramble_overlay_attach()` helper every
  MMIO-overlay model uses.
- `bramble_attach.c` / `.h`: the single machine-init fan-out. `bramble_attach()`
  is the one call the machine-init patch adds; it calls each model's attach in
  turn, threading through the GDMA and interrupt-matrix handles (the SoC state is
  private to `esp32s3.c`).
- `bramble_gpio.c` / `.h`: GPIO observer/injector (buttons, GNSS_EN, LED / vibra
  outputs, the radio soft CS, DIO1). Exposes the shared pin accessors the other
  models read/drive.
- `bramble_adc.c` / `.h`: SAR ADC oneshot stub so `battery_read_mv()` completes.
- `bramble_gpspi2.c` / `.h`: the GPSPI2 (SPI2_HOST) controller only - register
  file, SPI_USR/done, SSI bus, GDMA data path, CS routing. `bramble_gpspi2_attach`
  also instantiates the two bus slaves below.
- `bramble_sx1262.c` / `.h`: the SX1262 LoRa radio SSI slave (opcode state
  machine, RX FIFO, TX/RX/DIO1). Registered by its own `type_init`.
- `bramble_ssd1680.c` / `.h`: the SSD1680 e-paper SSI slave (command decode,
  image RAM, framebuffer unpack, `fb` emit). Registered by its own `type_init`.
- `bramble_indicators.c` / `.h`: the LEDC buzzer model plus the LED / vibra /
  buzzer -> emu-link `ind` bridge.
- `bramble_emulink.c` / `.h`: the emu-link transport (chardev, hello, JSON
  dispatch, the `tx`/`fb`/raw send helpers) to the gosim ether.

See `docs/archive/plans/emulator-phase2-qemu-spec.md` for the full spec and
`emulator/qemu/README.md` for the build/run instructions.
