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
