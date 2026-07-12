# Wiring patches

Patches here are applied to the espressif/qemu tree by `bootstrap-qemu.sh`
with `git apply` (idempotency guard: it checks whether the patch is already
applied and skips it rather than erroring). They do NOT carry the model source
files themselves (those are copied into `hw/xtensa/bramble/`, see
`../models/README.md`).

There are now just two "moves" that wire every bramble model in, plus two
stock-QEMU fixes. Adding a new device model needs NO new patch: drop its source
in `../models/`, list it in `../models/meson.build`, and add its attach call in
`bramble_attach.c`. This replaces the old per-model patch series (scaffold /
gpio / gpspi2 / adc / emulink), each of which edited BOTH `hw/xtensa/meson.build`
and `esp32s3.c:machine_init`, hand-placed to dodge its siblings' `git apply`
context windows - an O(N^2) fragility that bit nearly every model task.

Because each patch now touches a DIFFERENT file, none of them share context, so
placement collisions are gone entirely.

- `0001-bramble-subdir-meson.patch`: adds one `subdir('bramble')` line to
  `hw/xtensa/meson.build`. That reaches the bramble-owned
  `hw/xtensa/bramble/meson.build` (copied in, not patched), which lists every
  bramble model source. A new model is a new line in that file we own, never a
  patch to stock meson again.
- `0002-bramble-attach-machine-init.patch`: adds one
  `#include "hw/xtensa/bramble_attach.h"` and one
  `bramble_attach(get_system_memory(), DEVICE(&ss->gdma), DEVICE(&ss->intmatrix))`
  call to `esp32s3_machine_init()`, at the end of init where the GDMA and
  interrupt matrix the models need are already realized. `Esp32s3SocState` stays
  private to `esp32s3.c`; the handles the models need are threaded through
  `bramble_attach`'s parameters. The shim (`../models/bramble_attach.c`) fans out
  to every model's own attach function.
- `0003-fix-disable-slirp-not-found-propagation.patch` (build fix, stock QEMU):
  on this pinned tag, `-Dslirp=disabled` does not actually keep `net/slirp.c`
  out of a `have_system` build. Top-level `meson.build` wraps the (correctly
  not-found) `slirp_dep` in `declare_dependency()` unconditionally;
  `declare_dependency()` always reports `found() == true` regardless of its own
  `dependencies:` contents (verified: it does not propagate not-found), so
  `net/meson.build`'s `when: slirp` still pulls in `slirp.c`, which fails to
  compile without `libslirp.h`. This patch adds the missing `if
  slirp_dep.found()` guard so `--disable-slirp` actually disables it, matching
  the host constraint documented in README.md (libslirp is not available as a
  pkg-config module here; we use chardev sockets, not user-mode networking).
  Not a model wiring patch; kept on its own.
- `0004-intmatrix-level-forward.patch` (stock QEMU correctness fix): patches the
  STOCK esp32s3 interrupt matrix (`hw/xtensa/esp32s3_intc.{c,h}`, not a model
  file) to be combinational. The stock matrix only forwarded a source's level to
  a CPU on an input EDGE and dropped it on a routing-map write, so a peripheral
  whose level interrupt latches before the driver routes it (via
  `esp_intr_alloc`) never reached the CPU, and rerouting a source away from a
  line (as `esp_intr_disable` does) left the old line stuck asserted. This
  blocked the SX1262 radio, whose driver uses the interrupt-driven
  `spi_device_transmit` and relies on the latched SPI2 trans-done line:
  `radio_init` wedged in `spi_device_get_trans_result`. The patch tracks
  per-source input levels and re-drives each CPU interrupt as the OR of the
  sources routed to it on both input edges and map writes, letting the radio's
  transfers complete and boot reach the main loop. This is not a model wiring
  patch; it applies to files no other bramble patch touches.
