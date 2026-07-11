# Bramble Pager v1

A full Bramble mesh node (TX-capable; the mesh has no receive-only role) built into a
90s-pager formfactor: always-readable 2.13" e-paper, 1S LiPo with USB-C charge and
load-share, magnetic buzzer plus coin vibration motor, and a 3D-printed enclosure. It
runs stock Bramble firmware as a new board target (`bramble_pager`) that reuses every
Heltec-V3 pin assignment legal on an ESP32-S3-WROOM-1, adds an SSD1680 e-paper display,
and carries an ATGM336H GNSS receiver. Board is ~96x50mm landscape, 2-layer 1.6mm,
built for JLCPCB economic PCBA. This directory is the source of truth for the design.

The full requirement set, pin map, block-by-block rationale, and firmware deltas live in
`DESIGN.md`. This README covers the workflow: where truth lives, how to validate, what
must never regress, and how to bring a first article up.

## Sources of truth (ranked)

When two artefacts disagree, the higher entry wins. Never argue with ERC or DRC; fix the
schematic or the doc.

| Rank | Artefact | Authority |
|---|---|---|
| 1 | `kicad/*.kicad_sch` | Canonical electrical netlist. If a doc says a pin is wired to X and the schematic says Y, the schematic is correct. |
| 2 | `kicad/pager.kicad_pcb` | Canonical physical placement and routing. Authoritative for positions, layers, copper geometry. |
| 3 | `DESIGN.md` | Requirements, pin map, block rationale, do-not-regress invariants. |
| 4 | `COMPONENTS.md` | Per-component catalogue: value, package, LCSC part, nets, purpose, datasheet pinout citation. |
| 5 | `NET_TOPOLOGY.md` | Net-by-net prose for the load-bearing nets: why each exists, how each block is wired. |

`COMPONENTS.md` and `NET_TOPOLOGY.md` are derived docs. They mirror the schematic; if
either disagrees with a `.kicad_sch`, the schematic is right and the doc is a bug.

## File map

```
hardware/pager/v1/
├── README.md            # this file
├── DESIGN.md            # authoritative spec: requirements, pin map, blocks, invariants
├── PLAN.md              # implementation plan
├── COMPONENTS.md        # per-component catalogue (derived)
├── NET_TOPOLOGY.md      # net-by-net wiring prose (derived)
├── ERRATA.md            # errata + pre-fab open items
├── case/                # OpenSCAD enclosure
└── kicad/
    ├── pager.kicad_pro  # KiCad project
    ├── pager.kicad_sch  # root sheet (hierarchical references only)
    ├── power.kicad_sch  # USB-C, charger, protection, load-share, LDO
    ├── radio.kicad_sch  # LoRa module, GNSS module, both RF feeds
    ├── epaper.kicad_sch # display connector + boost / charge-pump
    ├── mcu.kicad_sch    # ESP32-S3, buttons, buzzer, vibra, LED, I2C header
    ├── pager.kicad_pcb  # PCB layout
    ├── pager.kicad_dru  # hard DRC backstops (RF, power-rail widths)
    └── easyeda/         # imported footprints/symbols (dimension-verified)
```

## Validation gates

Both gates use `kicad-cli` (not the KiCad MCP). MCP ERC/DRC read stale in-memory state
and have missed real violations; they are never authoritative here.

### Schematic ERC

```bash
kicad-cli sch erc --severity-error --exit-code-violations \
  hardware/pager/v1/kicad/pager.kicad_sch -o /tmp/erc.rpt
```

Must exit 0 (zero errors). The committed `kicad/pager-erc.rpt` is the last clean run.

### PCB DRC

```bash
kicad-cli pcb drc --refill-zones \
  hardware/pager/v1/kicad/pager.kicad_pcb -o /tmp/drc.rpt
```

Must report 0 unconnected, 0 clearance, 0 dangling. `--refill-zones` is mandatory;
without it DRC reads stale zone fills and misses zone-island and same-layer clearance
violations. The `.kicad_dru` net-name patterns must be re-audited against the actual
hierarchical net names (`/sheet/NET`) before every fab; a pattern that silently voids
against a renamed net gives a false-clean pass (the Digits v2 SW_NODE lesson).

## Do-not-regress invariants

Copied from `DESIGN.md`. Regressing any of these is a production defect.

- Every non-stdlib pinout and every pin-numbering orientation must be verified against
  the manufacturer datasheet (page/figure cited in `COMPONENTS.md`). EasyEDA part data,
  vendor symbol drawings, and "it is usually like this" do NOT count as verification.
  Claims that could not be datasheet-confirmed are tracked as open risks, not assumed.
- CC1 and CC2 each get their own 5.1k, never shared.
- Battery sense divider taps BAT+ (cell side), not VSYS.
- RESE resistor is 2.2 ohm 1% 0805 with kelvin sense to panel pin 3.
- EPD connector netlist is mirrored (connector N = panel 25-N); paper-mockup gate
  before gerbers.
- Parts under the panel deck rect stay <=3.2mm tall; the deck carries the glass 4mm
  above the PCB. The enclosure module-pocket interference check (SCAD asserts + rendered
  previews) is the physical gate, superseding the old 1mm glass keepout.
- RF trace: CPW, <5mm, stitched ground, no crossings; u.FL at case-wall edge.
- WROOM antenna keepout all layers.
- TCXO voltage 2.7V for this module (not 1.7V).
- 0x9D firmware flag lands + is bench-verified before boards are ordered.
- Netclass patterns audited against hierarchical net names (`/sheet/NET`) before fab.
- Gates: `kicad-cli sch erc --severity-error --exit-code-violations` = 0;
  `kicad-cli pcb drc --refill-zones` = 0/0/0 (never MCP ERC/DRC).

## Bring-up checklist

Order matters. Do not order boards until the pre-fab gates (below and in `ERRATA.md`)
are closed.

Before gerbers:

1. **FPC 1:1 paper mockup gate.** Print the panel tail + 180-degree fold + FH34SRJ-24S
   connector at 1:1. Confirm the mirrored pin order physically lands (connector pin N to
   panel pin 25-N). The whole EPD netlist depends on this fold geometry being right.
2. **DIO2 RF-switch flag.** Confirm the `radio_dio2_rf_switch` firmware flag has landed
   and been bench-verified on a Heltec V3 A/B test. Without the one-time 0x9D
   (`SetDio2AsRfSwitchCtrl`, payload 0x01) this module's chip-internal RF switch never
   arms and TX is dead.
3. **WROOM antenna copper keepout.** Confirm there is no copper pour on any layer under
   the WROOM-1 PCB-antenna zone (the module pads start ~4mm inside the left board edge;
   the antenna sits on-board over the keepout, it does not overhang the edge).

At first-article bring-up:

3. **DMM battery polarity check on every cell before first plug.** BATT1 pin 1 = BAT+
   (Adafruit convention). Reversed Chinese cells kill the charger. This is a per-unit
   assembly invariant, not a one-time check.
4. **TCXO voltage verify.** The module's TCXO is 2.8V nominal; firmware sets the nearest
   SX1262 code, 2.7V. Confirm the radio brings up and TXs cleanly at 2.7V; if not, try
   3.0V. Bench against a known-good Heltec V3 for a reference RSSI.
5. **NMEA talker-ID check.** The ATGM336H is CASIC/AT6558 (GPS+BDS) and emits `$GN`/`$GB`
   talker IDs, not just `$GP`. Confirm the firmware NMEA parser accepts them, and that
   `$GPTXT` antenna OPEN/SHORT reporting reads sane against the active GNSS antenna.

## Firmware build

The pager target is already wired into `scripts/flash.sh`. From the firmware repo root:

```bash
bash scripts/flash.sh local bramble-pager build
```

This applies `sdkconfig.bramble-pager` +
`SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.bramble_pager"` into
`build-bramble-pager/`. The output image is `build-bramble-pager/bramble.bin`. Console is
USB-Serial-JTAG (`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`), no UART bridge; flash and
monitor over the native USB-C. Board profile is `main/boards/bramble_pager.h`.

Equivalent direct `idf.py` form (use only for low-level control):

```bash
idf.py -B build-bramble-pager \
  -D SDKCONFIG=sdkconfig.bramble-pager \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.bramble_pager" \
  build
```
