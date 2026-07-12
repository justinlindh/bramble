# Wiring patches

Patches here are applied to the espressif/qemu tree by `bootstrap-qemu.sh`
with `git apply` (idempotency guard: it checks whether the patch is already
applied and skips it rather than erroring). They wire the model sources
injected from `../models/` into the esp32s3 machine and the meson build;
they do not carry the model source files themselves (those are copied, see
`../models/README.md`).

- `0001-inject-bramble-device-scaffold.patch` (P2.2-infra): adds
  `bramble_scaffold.c` to `hw/xtensa/meson.build`'s ESP32S3 source set and
  calls `bramble_scaffold_init()` from `esp32s3_machine_init()` in
  `hw/xtensa/esp32s3.c`, right after the SoC's peripherals are realized.
  Proves the injection mechanism; adds no device behavior. P2.2-P2.5 will
  add their own patches here (or extend this one) as real models land.
- `0002-fix-disable-slirp-not-found-propagation.patch` (P2.2-infra, build
  fix): on this pinned tag, `-Dslirp=disabled` does not actually keep
  `net/slirp.c` out of a `have_system` build. Top-level `meson.build` wraps
  the (correctly not-found) `slirp_dep` in `declare_dependency()`
  unconditionally; `declare_dependency()` always reports `found() == true`
  regardless of its own `dependencies:` contents (verified: it does not
  propagate not-found), so `net/meson.build`'s `when: slirp` still pulls in
  `slirp.c`, which fails to compile without `libslirp.h`. This patch adds
  the missing `if slirp_dep.found()` guard so `--disable-slirp` actually
  disables it, matching the host constraint documented in README.md
  (libslirp is not available as a pkg-config module here; we use chardev
  sockets, not user-mode networking).
- `0003-bramble-gpio-model.patch` (P2.2): wires `bramble_gpio.c` into the
  ESP32S3 source set and calls `bramble_gpio_attach()` from
  `esp32s3_machine_init()`, where the SoC state and its interrupt matrix are
  in scope (`Esp32s3SocState` is private to `esp32s3.c`, so the model cannot
  reach them from a separate file). The model overlays the GPIO register
  window to log output-pin transitions and inject button input; see
  `../models/bramble_gpio.c`. Its meson entry and machine-init call are placed
  clear of `0001`'s adjacent lines so both patches reverse-check as
  already-applied on an idempotent re-run.
- `0004-bramble-gpspi2-model.patch` (P2.3): wires `bramble_gpspi2.c` into the
  ESP32S3 source set and calls `bramble_gpspi2_attach()` from
  `esp32s3_machine_init()`, passing the realized GDMA (`ss->gdma`, for the
  framebuffer DMA data path) and the interrupt matrix. The GPSPI2 register
  window is unmodeled by the stock machine, so the pager's e-paper/radio SPI
  transfers spin on SPI_USR and boot wedges in `show_splash`; the model overlays
  that window and completes the transfers, unwedging boot. Its meson entry is
  placed near the top of the file and the machine-init call inside the crypto
  realization run, both clear of the `0001`/`0003` context lines, so all four
  patches reverse-check as already-applied on an idempotent re-run. See
  `../models/bramble_gpspi2.c`.
- `0005-bramble-adc-model.patch` (P2.3b): wires `bramble_adc.c` into the ESP32S3
  source set (its meson add() sits at the very head of the file, sharing only
  the `xtensa_ss = ss.source_set()` context line) and calls
  `bramble_adc_attach()` from `esp32s3_machine_init()`. The SENS/SAR window the
  oneshot ADC uses is unmodeled, so `battery_read_mv()` spins on the never-set
  `meas1_done_sar` bit; the model overlays that window and latches a conversion
  result. See `../models/bramble_adc.c`.
- `0006-intmatrix-level-forward.patch` (P2.4a): patches the STOCK esp32s3
  interrupt matrix (`hw/xtensa/esp32s3_intc.{c,h}`, not a model file) to be
  combinational. The stock matrix only forwarded a source's level to a CPU on an
  input EDGE and dropped it on a routing-map write, so a peripheral whose level
  interrupt latches before the driver routes it (via `esp_intr_alloc`) never
  reached the CPU, and rerouting a source away from a line (as `esp_intr_disable`
  does) left the old line stuck asserted. This blocked the SX1262 radio, whose
  driver uses the interrupt-driven `spi_device_transmit` and relies on the
  latched SPI2 trans-done line: `radio_init` wedged in `spi_device_get_trans_result`.
  The patch tracks per-source input levels and re-drives each CPU interrupt as
  the OR of the sources routed to it on both input edges and map writes, letting
  the radio's transfers complete and boot reach the main loop. This is the only
  patch that touches stock QEMU device code rather than wiring in a model. It
  applies to files no other bramble patch touches, so it is independent of the
  meson/machine-init hunk placement above.
