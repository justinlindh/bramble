# Bramble Pager v1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development
> (recommended) or superpowers:executing-plans to implement this plan task-by-task.
> Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Produce a fab-ready KiCad project (schematic ERC-clean, PCB DRC-clean, JLCPCB
production files), an OpenSCAD enclosure, and the blocking firmware deltas for the
Bramble Pager v1 defined in `hardware/pager/v1/DESIGN.md`.

**Architecture:** Hierarchical KiCad schematic (root + power/radio/epaper/mcu sheets),
project-local symbol/footprint libraries per the Digits v3 convention, 2-layer 50x90mm
board, JLCPCB economic PCBA. All electrical content (nets, values, LCSC numbers) is
specified in DESIGN.md; this plan sequences the work and pins the verification gates.

**Tech Stack:** KiCad 9 + kicad-cli, KiCad MCP server (per reliability table below),
kiutils/text-surgery fallback, Fabrication Toolkit, jlcsearch API, OpenSCAD, ESP-IDF.

## Global Constraints

- Source of truth for every net, value, and LCSC number: `hardware/pager/v1/DESIGN.md`
  (spec). If plan and spec disagree, the spec wins; update the spec first if it is wrong.
- ERC gate: `kicad-cli sch erc --severity-error --exit-code-violations <sch>` must exit 0.
  NEVER trust MCP run_erc.
- DRC gate: `kicad-cli pcb drc --refill-zones <pcb>` must report 0/0/0.
  NEVER trust MCP run_drc. `--refill-zones` is mandatory.
- MCP discipline: call `open_project` at the start of every MCP batch; grep the file for
  key refs after every MCP write; close any KiCad UI before MCP edits; on SWIG death
  ("SwigPyObject has no attribute") fall back to kiutils/text surgery immediately;
  never use `sync_schematic_to_board` (corrupts netlists), `refill_zones` (segfault),
  `delete_schematic_net_label` on global labels, or MCP pin-location coords (Y-inverted).
- Schematic wiring recipe: short wire stubs from pins via `add_schematic_wire`
  snapToPins=true + net labels at wire endpoints; never labels directly on pin coords.
- Every schematic component gets an `LCSC` field with the Cxxxxx from the spec.
- Reference designators: single tokens, no spaces (JLC CPL splits on whitespace).
- Custom footprint pad geometry must be verified against the datasheet land-pattern
  drawing, dimension by dimension, before use (Digits SW_DPDT lesson).
- Netclass patterns must use hierarchical net names (`/sheet/NET`); audit before fab.
- Em dashes are forbidden in all file content (user hook blocks them).
- Commit after every green gate; small commits on branch `pager-v1`.

## File Structure

```
hardware/pager/v1/
  DESIGN.md                    (committed spec)
  docs/archive/plans/pager-v1-plan.md   (this file)
  README.md                    (Task 10: sources of truth, invariants, gates)
  COMPONENTS.md                (Task 10: ref/part/package/LCSC/nets/purpose)
  NET_TOPOLOGY.md              (Task 10: net-by-net prose)
  kicad/
    pager.kicad_pro / .kicad_sch / .kicad_pcb / .kicad_dru
    power.kicad_sch  radio.kicad_sch  epaper.kicad_sch  mcu.kicad_sch
    pager.kicad_sym            (project symbol lib)
    pager.pretty/              (project footprint lib)
    production/                (Task 9: bom.csv, positions.csv, gerbers)
  case/
    pager_case.scad  (Task 11) + rendered STLs
  mockup/
    fpc-mockup-1to1.pdf        (Task 8 gate artifact)
firmware (separate PRs off main, not in this dir):
  components/board_config/include/board_config.h   (+ radio_dio2_rf_switch)
  components/radio/sx1262.c/.h                     (+ 0x9D command)
  main/boards/bramble_pager.h, main/board.c, main/Kconfig.projbuild,
  sdkconfig.defaults.bramble_pager, scripts/flash.sh
```

---

### Task 1: Firmware: DIO2-as-RF-switch flag (BLOCKING for board bring-up)

**Files:**
- Modify: `components/board_config/include/board_config.h` (radio section)
- Modify: `components/radio/include/sx1262.h`, `components/radio/sx1262.c`
- Modify: `components/radio/radio_esp.c` (init path)
- Test: `test/test_board_profiles.c` (host-buildable assertions)

**Interfaces:**
- Produces: `#define SX1262_CMD_SET_DIO2_AS_RF_SWITCH 0x9D`,
  `int sx1262_set_dio2_as_rf_switch(bool enable)`,
  `bool radio_dio2_rf_switch` field in `bramble_board_config_t` (default false via
  existing zero-init of static structs; heltec_v3/v4/tdeck structs unchanged).

- [ ] Step 1: Add `bool radio_dio2_rf_switch;` after `radio_reg` in board_config.h.
- [ ] Step 2: Add opcode + `sx1262_set_dio2_as_rf_switch(bool)` (write_command, 1-byte
      payload enable?1:0) to sx1262.c/.h, mirroring `sx1262_set_regulator_mode` style.
- [ ] Step 3: In radio_esp.c init sequence, after TCXO/regulator setup:
      `if (board_get_config()->radio_dio2_rf_switch) sx1262_set_dio2_as_rf_switch(true);`
- [ ] Step 4: Assert in test_board_profiles.c that heltec_v3 config has the flag false.
- [ ] Step 5: Build heltec-v3 target (`idf.py -B build-heltec-v3 -D SDKCONFIG=sdkconfig.heltec-v3 ... build`), expect success, zero behavior change.
- [ ] Step 6: Commit `feat(radio): optional SetDio2AsRfSwitchCtrl via board config`.
- [ ] Step 7 (user, later, non-blocking for schematic): A/B RSSI bench test on a V3 pair
      with flag on/off; if TX power jumps, file the fleet bug + enable for heltec_v3.

### Task 2: Firmware: bramble_pager board profile (skeleton)

**Files:**
- Create: `main/boards/bramble_pager.h`
- Modify: `main/Kconfig.projbuild`, `main/board.c` (both #if chains),
  `scripts/flash.sh`, Create: `sdkconfig.defaults.bramble_pager`

**Interfaces:**
- Produces: `CONFIG_BRAMBLE_BOARD_PAGER`, board struct with the DESIGN.md pin map
  (radio 8/12/13/14, SPI 9/10/11, battery GPIO1/CH0/factor2, button 0, tcxo 2.7f,
  `radio_dio2_rf_switch = true`, capabilities WITHOUT display for now: epaper cap
  arrives with the driver in a later PR; I2C 17/18).

- [ ] Step 1: Write bramble_pager.h copying heltec_v3.h shape; epaper pins noted in
      comments until the driver lands; `BOARD_CAP_BATTERY_ADC` only.
- [ ] Step 2: Kconfig choice + board.c branches + flash.sh case + sdkconfig overlay
      (`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`, 8MB flash).
- [ ] Step 3: `idf.py -B build-pager -D SDKCONFIG=sdkconfig.pager -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.bramble_pager" build` passes.
- [ ] Step 4: Commit `feat(board): bramble_pager target skeleton`.

### Task 3: KiCad project scaffold + design rules

**Files:**
- Create: `hardware/pager/v1/kicad/pager.kicad_pro`, root `pager.kicad_sch`,
  `pager.kicad_pcb`, `pager.kicad_dru`, `sym-lib-table`, `fp-lib-table`

- [ ] Step 1: `mcp create_project` (or copy digits v3 skeleton) at kicad/; project-local
      lib tables with `${KIPRJMOD}` URIs for `pager.kicad_sym` / `pager.pretty`.
- [ ] Step 2: Netclasses in .kicad_pro: Default 0.2/0.13, Power 0.4 (patterns `+3V3`,
      `GND`, `VBUS`, `VSYS`, `/power/BAT_PLUS`, `/power/BAT_MINUS`), RF 1.0
      (pattern `/radio/RF_ANT`). Patterns use hierarchical prefixes.
- [ ] Step 3: pager.kicad_dru hard rules: RF_ANT min 1.0mm + front-only + no vias;
      power nets min 0.4mm (backstop for netclass silent-void).
- [ ] Step 4: ERC gate on empty root sheet passes; commit `feat(pager): kicad scaffold`.

### Task 4: Symbols + footprints (the long pole; parallelizable per part)

**Files:**
- Modify: `kicad/pager.kicad_sym`, Create: `kicad/pager.pretty/*.kicad_mod`

For each part: prefer KiCad stdlib symbol/footprint; else import from LCSC/easyeda2kicad;
else draw from datasheet. Verify every non-stdlib footprint against the datasheet
land pattern dimension-by-dimension; record verification note in COMPONENTS.md draft.

- [ ] ESP32-S3-WROOM-1 (stdlib `RF_Module:ESP32-S3-WROOM-1` + matching footprint).
- [ ] NiceRF LoRa1262 (custom: 16 castellated pads, 2.0mm pitch, 16x16mm; from datasheet).
- [ ] FH34SRJ-24S (stdlib `Connector_FFC-FPC` has FH34SRJ-24S footprint; verify pad drawing).
- [ ] TYPE-C-31-M-12 (stdlib HRO footprint `Connector_USB:USB_C_Receptacle_HRO_TYPE-C-31-M-12`).
- [ ] TP4056 ESOP-8 (SOIC-8-EP footprint; verify EP size), DW01A SOT-23-6, FS8205A SOT-23-6,
      XC6220 SOT-23-5, USBLC6 SOT-23-6, AO3400A/AO3401A SOT-23, 2N7002 not fitted.
- [ ] MLT-8530 (custom from datasheet), TS-1187A (custom or stdlib side tact; verify),
      JST PH S2B-PH-SM4 RA + ZH B2B/B4B-ZR-SM4 (stdlib JST libs; verify variants),
      u.FL BWIPX-1-001E (stdlib `Connector_Coaxial:U.FL_Hirose_U.FL-R-SMT-1`),
      B5819W SOD-123, SS14/SS34 SMA, passives 0603/0805, LEDs 0603, L 4030 inductor.
- [ ] Gate: every symbol has LCSC field; every custom footprint has a written
      dimension check. Commit per part group.

### Task 5: Hierarchical schematic capture

**Files:**
- Modify: root `pager.kicad_sch`; Create: `power.kicad_sch`, `radio.kicad_sch`,
  `epaper.kicad_sch`, `mcu.kicad_sch`

Sheet contents are DESIGN.md sections verbatim (Power, Radio+RF path, Display, HMI+MCU;
HMI lives on the mcu sheet). Inter-sheet nets via hierarchical labels: +3V3, GND, VBUS,
VSYS, BAT_PLUS, SPI_SCK/MOSI/MISO, RADIO_*, EPD_*, I2C_*, VBAT_SENSE, alert gates.

- [ ] Step 1: Root sheet with 4 sheet symbols + global power flags + PWR_FLAG.
- [ ] Step 2: power.kicad_sch (USB-C, ESD, charger, protection, load share, LDO,
      battery connector, sense divider). Semantic refdes (USB1, CHG1, BATT1, ...).
- [ ] Step 3: radio.kicad_sch (module, decoupling, u.FL, RF_ANT net).
- [ ] Step 4: epaper.kicad_sch (FPC connector with MIRRORED pin mapping per spec,
      boost/charge pump, rail caps, BS1 strap).
- [ ] Step 5: mcu.kicad_sch (WROOM, EN circuit, buttons, buzzer/vibra drivers, LED,
      I2C header + pullups, USB D+/D- to connector nets).
- [ ] Step 6: `kicad-cli sch export netlist` and audit: every DESIGN.md net present,
      pin-level spot check of the 6 invariant nets (CC1/CC2, RESE, BAT sense tap,
      BS1, VDDIO tie, DIO3 unconnected + no_connect flags on all NC pins).
- [ ] Step 7: ERC gate exits 0. Export SVG (`kicad-cli sch export svg`) and visually
      review every sheet (real render review, not tool output faith).
- [ ] Step 8: Commit `feat(pager): schematic capture` (+ checkpoint snapshot).

### Task 6: PCB setup, placement

**Files:**
- Modify: `kicad/pager.kicad_pcb`

- [ ] Step 1: Board outline 50x90mm rounded corners; mounting holes 4x M2.
- [ ] Step 2: Import netlist. Preferred: KiCad GUI "Update PCB from schematic" via
      `launch_kicad_ui` if MCP path is unreliable; NEVER `sync_schematic_to_board`.
      Audit: no duplicate unprefixed nets (`grep -c 'MIRROR\|/power/' pager.kicad_pcb`
      style checks; compare net count to netlist).
- [ ] Step 3: Placement per DESIGN.md mechanical: USB-C centered bottom edge (case
      datum), WROOM top with antenna overhang, NiceRF + u.FL at top-left edge
      (case wall), EPD connector + boost under panel zone (nothing >1mm tall there),
      buttons on side edges, buzzer bottom area away from panel, TP4056 away from
      radio, JST-PH near battery bay.
- [ ] Step 4: Export placement view (`export_svg` / `get_board_2d_view` alternatives:
      kicad-cli pcb export svg) and visually verify constraints; commit.

### Task 7: Routing + DRC gate

- [ ] Step 1: Route RF_ANT first, manually: CPW 1.0-1.2mm, <5mm, stitching vias.
- [ ] Step 2: Route power nets (0.4mm+) + EPD boost loop tight + RESE kelvin.
- [ ] Step 3: USB D+/D- as 90-ohm-ish short pair, no stubs.
- [ ] Step 4: Remaining signals: MCP route_trace/route_pad_to_pad, or Freerouting via
      DSN export (digits v2 flow) with *-pre-freeroute snapshot first.
- [ ] Step 5: GND pours both layers, stitching vias; thermal vias under TP4056 EP.
- [ ] Step 6: DRC gate 0/0/0 with --refill-zones. Fix loop until clean.
- [ ] Step 7: Pre-fab audit (digits checklist): netclass patterns vs live net names,
      per-net current budget, EP thermal via counts, unconnected-net subgraph check
      (highlight +3V3/GND), silk polarity marks (battery!), component-side vs case.
- [ ] Step 8: Commit `feat(pager): layout + routing, DRC clean`.

### Task 8: FPC mirror mockup gate (physical, user-in-loop)

- [ ] Step 1: Export 1:1 PDF of the board top (kicad-cli pcb export pdf) + a one-page
      instruction sheet: print, cut panel outline (59.2x29.2mm active + tail), fold
      tail under, align to connector footprint, confirm pin 1 arrives at connector
      pad 24 (mirror) with contacts facing DOWN into the flip connector.
- [ ] Step 2: USER ACTION: perform mockup, confirm. Blocks gerber generation only;
      everything else proceeds.

### Task 9: Production files

- [ ] Step 1: Fabrication Toolkit run (config JSON copied from digits v3) into
      kicad/production/: bom.csv, positions.csv, gerbers.
- [ ] Step 2: BOM audit by hand against DESIGN.md LCSC table (header exactly
      "LCSC Part #"); rotation fixes in CPL (SOT-23 +180, QFN +90, SOIC +90/270).
- [ ] Step 3: jlcsearch re-verify stock of all extended parts at order time.
- [ ] Step 4: Commit bom.csv only (digits convention) + docs.

### Task 10: Documentation set

- [ ] README.md (sources-of-truth table, invariants from DESIGN.md, gates, build cmds),
      COMPONENTS.md (every ref: part/package/LCSC/nets/purpose/footprint-verification),
      NET_TOPOLOGY.md (net-by-net prose with datasheet citations), ERRATA.md (empty).
- [ ] Commit `docs(pager): v1 documentation set`.

### Task 11: OpenSCAD enclosure

**Files:**
- Create: `hardware/pager/v1/case/pager_case.scad` (+ STLs)

- [ ] Step 1: Parametric model keyed off pcb dims + connector datum: shell 65x100x18mm,
      wall 2.0mm, PCB standoffs, USB-C flush port (8.94x3.26mm + 0.3mm clearance,
      outward chamfer, local wall recess), e-paper window + bezel, 4 side button
      holes at TS-1187A actuator positions (from PCB placement coords), battery bay
      34x62x6.5mm with foam ribs, vibra pocket 10.2mm, antenna channel on inner wall
      (79x10mm flat zone, no metal), lid with snap fits, belt clip boss.
- [ ] Step 2: `openscad -o case.stl` renders without errors; visual check of section
      views (`projection()` exports or camera renders).
- [ ] Step 3: Commit `feat(pager): printable enclosure v1`.

### Task 12: Finish branch

- [ ] Push `pager-v1`, open PR (schematic SVGs + board render in description),
      /code-review before merge per merge policy; firmware tasks 1-2 may ship as
      their own PR earlier.

## Self-Review

- Spec coverage: Tasks 1-2 cover firmware deltas 1-2 (delta 3 e-paper driver and
  delta 4 alerts are explicitly firmware-later per spec, not needed for fab). Tasks
  3-9 cover schematic->production. Task 8 covers the FPC mockup invariant. Task 11
  covers mechanical incl. the USB-C body-port requirement. Task 10 covers doc
  conventions. No gaps against DESIGN.md.
- Placeholders: none; net-level content intentionally lives in DESIGN.md (DRY).
- Consistency: pin map, net names, and gate commands match DESIGN.md exactly.
