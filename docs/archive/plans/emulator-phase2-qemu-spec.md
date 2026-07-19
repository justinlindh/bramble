# Bramble Emulator Phase 2: QEMU true-VM backend (spec)

Status: spec drafted 2026-07-11, kickoff approved by Justin ("let's move on to
phase 2 as outlined previously"). Phase 1 (soft device) is COMPLETE and is the
substrate this builds on; see DESIGN.md section 12 for the contract this phase
fulfills.

## 1. Goal

Run the EXACT flashable Bramble Pager firmware image (the same .bin esptool
would write to a physical board: real Xtensa instructions, real FreeRTOS
scheduling, real drivers including sx1262.c and ssd1680_io.c over SPI) inside
QEMU, attached to the existing gosim ether as just another external node. The
browser device view, scenarios, and CI do not change: a QEMU node emits the
same emu-link messages (hello/tx/fb/ind/...) the linux node does, produced by
QEMU device models instead of virtual drivers.

What this buys over phase 1: the linux node compiles firmware SOURCE for the
host; QEMU executes the shipped BINARY. Driver bugs (SPI sequencing, the
SSD1680 command stream, SX1262 register handling, ISR timing) become testable
before hardware, and the emulator's fidelity claim upgrades from "same logic"
to "same image".

## 2. Ground truth (verified 2026-07-10 research, re-verify at P2.1)

- Espressif maintains the QEMU fork (github.com/espressif/qemu, esp-develop
  releases, esp-develop-9.2.2 era). ESP32-S3 machine exists: dual-core Xtensa,
  UART, interrupt matrix, NOR flash + MMU, PSRAM, eFuse (file-backed), RNG,
  GDMA, AES/SHA/RSA/HMAC, systimer, timer groups + WDTs.
- NOT emulated on S3 (the gap this phase fills): GPSPI2/3 (spi_master), the
  GPIO matrix/IOMUX (boot straps only), I2C, LEDC, ADC, USB-OTG. Secure Boot
  unsupported on S3; deep-sleep behavior undocumented.
- `idf.py qemu` integration exists for the dev loop; QEMU chardev sockets are
  the bridge for custom device models to reach the outside world.
- Nobody has published register-accurate SX1262 or SSD1680 QEMU models; we
  write the first, informed by our own drivers and the datasheet work already
  in-tree (ssd1680_engine tests pin the exact command vocabulary the model
  must accept).

## 3. Architecture

```
 QEMU esp32s3 machine (runs bramble.bin verbatim)
 +---------------------------------------------------+
 |  firmware: real sx1262.c / ssd1680_io.c / gps.c   |
 |     |            |              |          |       |
 |  GPSPI2 ctrl  GPIO model     UART1      (existing  |
 |  (new C model)  (new)        (exists)    UART0)    |
 |     |            |              |                  |
 |  SSI bus      button/gate    NMEA in               |
 |   +-- SX1262 slave model --> chardev socket --+    |
 |   +-- SSD1680 slave model -> chardev socket --+    |
 +-----------------------------------------------|----+
                                                 v
                              qemu-shim (small Go or C adapter)
                                                 |
                                        emu-link (unix socket)
                                                 |
                                           gosim broker
```

- The device models translate register/FIFO activity into emu-link semantics:
  SX1262 model: SetDio2AsRfSwitchCtrl, buffer read/write, TX -> emu-link tx,
  emu-link rx -> DIO1 IRQ + buffer; SSD1680 model: RAM writes + Master
  Activation -> fb message (kind from Display Update Control 2 value, the same
  0xF7/0xFF distinction the engine tests pin).
- One adapter process per QEMU node bridges chardev JSON to the broker socket
  and injects btn/nmea/batt as GPIO/UART stimuli via QMP or a control chardev.
  (Whether the models speak emu-link JSON directly or via the shim is a P2.4
  design decision; start with the shim, it keeps the C models dumb.)
- gosim: a scenario node type {"type": "qemu", "image": ...} whose supervisor
  entry spawns qemu-system-xtensa with the right -device/-chardev wiring.
  Everything downstream (positions, airtime, collisions, device view, E2E) is
  unchanged.

## 4. Image build for QEMU

The stock pager build signs for Secure Boot and expects flash encryption
workflows; QEMU S3 supports neither. Add sdkconfig.defaults.qemu layered on
the pager defaults: signing/encryption off, console on UART0, everything else
identical. The claim "same image" means same code paths and binary layout,
with the security envelope necessarily relaxed; document this honestly
wherever the fidelity claim is made.

## 5. Milestones (each gets the phase-1 treatment: TDD where testable,
   independent review, honest gates)

- P2.1 QEMU base spike (GO/NO-GO): build espressif/qemu locally, build the
  pager image with the qemu sdkconfig variant, boot it in the stock esp32s3
  machine. Expected: boots through early init, wedges at the first GPSPI/GPIO
  touch. Deliverable: exact wedge points, UART log, a written GO/NO-GO on the
  QEMU base version to pin, and the dev-loop recipe (build + run + gdb).
- P2.2 GPIO model: enough of the GPIO matrix for buttons (0/21/47), GNSS_EN
  (38), LED/buzzer/vibra outputs (48/15/16) observed as events, and the boot
  straps. Buttons injectable (QMP or control chardev).
- P2.3 GPSPI2 controller model: the spi_master register/DMA surface the
  IDF driver actually uses (command/address/dummy/data phases, CS control,
  GDMA handoff), with an SSI-style bus to attach slave models. Scope strictly
  to what sx1262.c + ssd1680_io.c exercise; log-and-ignore the rest.
- P2.4 SX1262 slave model + shim: register-accurate for the command subset the
  driver uses (sleep/standby/set-packet/buffer IO/TX/RX/CAD/IRQ/DIO2-RF-switch
  0x9D), bridging to emu-link tx/txdone/rx/cadres. Acceptance: the QEMU node
  attaches to gosim, beacons, and exchanges channel messages with linux nodes
  in one scenario.
- P2.5 SSD1680 slave model: accepts the exact op stream ssd1680_engine emits
  (the engine tests double as the model's conformance spec), maintains BW RAM,
  emits fb on Master Activation with kind/busy semantics, models BUSY timing
  (this also closes the phase-1 white-flash fidelity gap on the QEMU path).
  Acceptance: boot screen + message render in the browser from a QEMU node,
  pixel-identical to the linux node for the same content.
- P2.6 Integration: gosim "qemu" node type + mixed scenarios (qemu + linux +
  harness nodes on one ether); UART1 NMEA injection for GPS; the E2E suite
  runs against a QEMU-backed scenario as a parity gate.
- P2.7 Packaging: make targets (make qemu-node, make run-qemu), Docker layer
  with qemu-system-xtensa, CI job (heavy: non-required, nightly-ish cadence),
  docs, and the honest fidelity statement (what is identical, what is
  approximated, what is off).

## 6. Risks

1. QEMU fork churn / S3 machine gaps beyond the known list (P2.1 exists to
   surface these; pin a known-good release tag).
2. GPSPI+GDMA model complexity: the IDF spi_master driver uses DMA descriptors
   and interrupt timing; scope discipline (only what our two drivers use) is
   the mitigation, and the linux-node path remains the fast dev loop.
3. Timing realism: QEMU gives instruction-level execution, not cycle accuracy;
   busy-wait heavy drivers may behave differently. Acceptable: fidelity target
   is functional, not cycle-exact.
4. Wall-clock cost: a QEMU node is heavier than a linux node; CI treats QEMU
   scenarios as the slow tier, linux nodes stay the default.
5. This is C systems work inside a large foreign codebase (QEMU); reviews for
   P2.2-P2.5 need reviewers reading QEMU idioms (qdev/SSI/chardev APIs), not
   just our tree.

## 7. Exit criteria

A mixed scenario (1 QEMU pager + 2 linux pagers) where the QEMU node runs the
signed-off qemu-variant image, exchanges channel messages both directions,
renders its e-paper in the browser device view with BUSY-accurate refresh
kinds, accepts button/GPS/battery stimuli, survives reset-with-identity, and
the E2E parity gate passes against it; plus the P2.7 packaging and docs.
