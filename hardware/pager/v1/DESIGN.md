# Bramble Pager v1 - Design Spec

Status: **approved 2026-07-10** (component selection verified live against JLCPCB stock that day).
A full Bramble mesh node (TX-capable; the mesh has no receive-only role) in a 90s-pager
formfactor: always-readable 2.13" e-paper, LiPo + USB-C, buzzer + vibration motor,
3D-printed enclosure. Target: JLCPCB 2-layer economic PCBA, ~50x90mm board.

Follows the Digits v3 hardware conventions (`~/src/digits/hardware/pcb/v3`): per-rev
directory, sources-of-truth ranking, COMPONENTS.md / NET_TOPOLOGY.md as derived docs,
`.kicad_dru` hard rules, Fabrication Toolkit production output, kicad-cli ERC/DRC gates.

## Sources of truth (ranked)

1. `kicad/*.kicad_sch`: electrical intent
2. `kicad/*.kicad_pcb`: physical layout
3. This file: requirements, pin map, invariants
4. `COMPONENTS.md` / `NET_TOPOLOGY.md`: derived documentation (written during schematic phase)
5. Vendor datasheets (linked per part)

## Requirements

- Runs Bramble firmware as a new board target (`bramble_pager`) with minimal diff:
  reuse every Heltec-V3 pin assignment that is legal on a WROOM-1 module.
- Full LoRa TX/RX at US915 (mesh participation is mandatory: RREP, ACKs, beacons).
- Always-on readable display with hardware partial refresh; ~0 power when static.
- Battery: ~1200mAh 1S LiPo, USB-C charge at 500mA, charge-while-running (load share),
  protection for unprotected cells, fuel gauge via 2:1 divider (firmware `divider_factor=2`).
- Pager alerts: magnetic buzzer + coin vibration motor (hardware now, firmware later).
- HMI: BOOT/SELECT + UP + DOWN user buttons, RESET, status LED, all case-edge actuated.
- Native USB (S3 USB-Serial-JTAG) for console + flashing; no UART bridge chip.
- Antenna inside the case (u.FL to flex antenna on case wall).
- Deep-sleep floor <=100uA to enable future wake-on-DIO1 pager duty cycling
  (`bramble.sleep` RPC already arms ext0 wake on DIO1).

## Pin map (ESP32-S3-WROOM-1)

Heltec-V3-matching pins marked `*`.

| Function | GPIO | Notes |
|---|---|---|
| SPI SCK | 9* | shared bus: radio + e-paper, mode 0, 8MHz |
| SPI MOSI | 10* | |
| SPI MISO | 11* | |
| Radio NSS | 8* | active low |
| Radio NRESET | 12* | active low |
| Radio BUSY | 13* | polled |
| Radio DIO1 | 14* | rising-edge IRQ; also ext0 deep-sleep wake |
| EPD CS | 4 | |
| EPD D/C | 5 | |
| EPD RES# | 6 | active low |
| EPD BUSY | 7 | **active HIGH** (SSD1680; wait-for-low) |
| I2C SDA | 17* | debug header; 10k pullups fitted on board |
| I2C SCL | 18* | |
| VBAT sense | 1* | ADC1_CH0; 100k/100k divider + 100nF, sensed at BAT+ (before load-share FET) |
| BOOT/SELECT btn | 0* | to GND, strapping-canonical |
| UP btn | 21 | to GND, internal pullup |
| DOWN btn | 47 | to GND, internal pullup |
| Buzzer gate | 15 | AO3400A low-side, LEDC PWM ~2.7kHz |
| Vibra gate | 16 | AO3400A low-side |
| Status LED | 48 | 1k series, active high |
| USB D- | 19 | native USB, 90 ohm diff pair |
| USB D+ | 20 | |
| GNSS TX | 35 | ESP -> module RXD |
| GNSS RX | 36 | ESP <- module TXD |
| GNSS_EN | 38 | P-FET gate, LOW = GNSS on |
| RESET btn | EN | shorts EN to GND; EN has 10k pullup + 1uF |

Reserved/avoided: 3, 45, 46 (strapping, unused), 26-32 (flash, not bonded), 37
(unused), 39-42 (kept free for JTAG), 43/44 (kept free for UART0 debug fallback).
N8R8 octal-PSRAM drop-in option was dropped 2026-07-10 when GPIO35/36 went to GPS.

## Blocks

### Radio: NiceRF LoRa1262-915TCXO (LCSC C5356643, extended, ~$6.54)

16 castellated pins, 2.0mm pitch, 16x16mm, shielded, FCC ID 2AD6-1262.
Pinout: 1 GND, 2 MISO, 3 MOSI, 4 SCK, 5 NSS, 6 NRESET, 7 NC, 8 GND, 9 **ANT**,
10 GND, 11 **DIO3: leave unconnected** (module powers its TCXO from DIO3 internally),
12 NC, 13 VCC (10uF + 100nF at pin), 14 NC, 15 DIO1, 16 BUSY.

- Board profile: `radio_osc = RADIO_OSC_TCXO_DIO3`, `radio_tcxo_voltage = 2.7f`
  (datasheet says 2.8V TCXO; nearest SX1262 codes 2.7/3.0, verify at bring-up;
  **not** Heltec V3's 1.7f). Regulator mode DC-DC.
- **FIRMWARE-BLOCKING**: RF switch is chip-internal on DIO2 (not exposed). Driver must
  send `SetDio2AsRfSwitchCtrl` (0x9D, payload 0x01) once at init or TX is dead.
  Add `bool radio_dio2_rf_switch` to `bramble_board_config_t`, default false, true here.
  Land + bench-verify before ordering boards.
- Fleet follow-up: Meshtastic sets this flag for Heltec V3; Bramble doesn't. A/B test
  V3 TX power (fixed-peer RSSI) with/without. May be a latent fleet TX bug.
- Alternate (rev-B fallback, do not fit): Ebyte E22-900M22S C411293. Needs TXEN/RXEN
  handling (DIO2 to TXEN, 2N7002 inverter to RXEN with 100k gate pulldown), TCXO 1.8V.

### RF path: u.FL BWIPX-1-001E (C496552) + Molex 105262-0001 flex antenna

- ANT (pin 9) to u.FL: **<5mm**, coplanar waveguide ~1.0-1.2mm trace / 0.5mm gap to
  top pour, solid L2 ground beneath, stitching vias every 2-3mm, nothing crossing under.
- u.FL at board edge nearest the case wall carrying the antenna.
- Antenna (DigiKey 1052620001, ~$5): 79x10mm adhesive flex, 100mm MHF1 pigtail,
  ground-plane independent. Mount on case wall, element clear of PCB/battery/copper.

### Display: GoodDisplay GDEY0213B74, SSD1680, 250x122 (hand-plugged; ~$6)

Connector: Hirose FH34SRJ-24S-0.5SH (C324726, extended): 0.5mm 24-pin, **dual
top+bottom contact**, back-flip actuator, accepts the 0.3mm panel tail.

Panel tail folds 180 degrees under the panel into the connector, so **pin order MIRRORS:
connector pin N = panel pin 25-N** (WeAct EpaperModule wiring confirms).
**GATE: 1:1 paper mockup of panel + fold + connector before gerbers.**

Panel pinout: 1 NC, 2 GDR, 3 RESE, 4 NC, 5 VSH2, 6 TSCL (open), 7 TSDA (open),
8 BS1 to **GND** (4-wire SPI), 9 BUSY (active high), 10 RES#, 11 D/C#, 12 CS#, 13 SCL,
14 SDA, 15 VDDIO tied to 16, 16 VCI=3V3, 17 VSS, 18 VDD (internal LDO, 1uF only),
19 VPP open, 20 VSH1, 21 VGH, 22 VSL, 23 VGL, 24 VCOM.

Boost/charge-pump (panel datasheet p.29 ref circuit, WeAct-proven basic substitutions):
- L1 47uH FNR4030S470MT (C167888, ext) from 3V3 (C4 4.7uF C1779) to switch node
- Q1 AO3400A (C20917, basic) drain=switch node, gate=GDR with R1 1M (C22935) pulldown,
  source to R2 **2.2 ohm** 0805 1% (C2933402, ext; this panel is NOT 0.47 ohm) to GND;
  RESE pin kelvin-sensed at R2/source node
- D3 B5819W (C8598, basic) switch node to VGH + C5; C3 4.7uF flying cap to D2/D1
  (B5819W x2) negative pump to VGL + C11
- 1uF/50V (C15849, basic) x8 on VSH2/VGH/VCI/VDD/VSH1/VSL/VGL/VCOM at the connector
- Init: SSD1680 (not UC8151); partial refresh via 0x11/0x44/0x45/0x4E/0x4F + 0x22/0x20;
  full refresh every ~5 partials; deep sleep 0x10 (~1uA, no load switch needed).
  GxEPD2_213_GDEY0213B74 is the reference driver.
- Layout: tight boost loop; **parts under the panel deck rect stay <=3.2mm tall** (the
  deck carries the glass 6mm above the PCB; WROOM, buzzer, switches clear it); the
  enclosure module-pocket interference check is the physical gate. FPC actuator faces
  away from the panel edge with finger room.
- Panel sourcing: GoodDisplay AliExpress store / buy-lcd.com / Laskakit; pin-compatible
  fallback DEPG0213BN (Heltec Wireless Paper panel). Order panels early.

### Power

- USB-C TYPE-C-31-M-12 (C165948): VBUS A4/A9/B4/B9, GND A1/A12/B1/B12, D+/- pairs tied.
  CC1/CC2 each get **their own** 5.1k (C23186) to GND. Shield to GND. 5V VBUS only, never PD.
- ESD: USBLC6-2SC6 (C2687116; genuine ST = C7519) within 5mm of connector; VBUS + D+/-.
- Charger: TP4056-42 ESOP-8 (C16581, preferred tier). PROG=2.2k (C4190) for 500mA
  (TopPower REV_2.4 datasheet p.5/p.9: constant is 1100 not 1200, and the EC table
  specs RPROG=2.2k as the 500mA point; 2.4k would give ~458mA). TEMP to GND, CE to VCC, VCC/BAT 10uF each. EP to GND pour +
  thermal vias (~1W worst case; keep away from radio/TCXO). CHRG/STDBY drive red C2286 /
  green C965805 LEDs via 1k (C21190) from VBUS.
- Protection (cell-agnostic, kept even with protected cells): DW01A (C351410, 100R+100nF)
  + FS8205A (C2830320) low-side: cell- to S1, S2 to board GND, OD to G1, OC to G2.
- Load share (Adafruit/AN1149 style): BAT+ to AO3401A (C15127) drain, source to VSYS,
  gate to VBUS with 100k (C25803) pulldown; SS34 (C8678) VBUS to VSYS.
  USB present: FET off, cell isolated, load runs from VBUS (VSYS~4.6V), termination clean.
- 3.3V: XC6220B331MR-G (C86534, genuine Torex only; avoid the $0.03 clone listing):
  1A, ~100mV dropout @500mA, 8uA IQ. 10uF in/out (C15850), CE to VIN.
- Battery: JST-PH S2B-PH-SM4-TB (C295747), pin 1 = BAT+ (Adafruit convention),
  prominent silkscreen. **Assembly invariant: DMM polarity check on every cell before
  first plug** (reversed Chinese cells kill the charger). Reference cell: Adafruit 258
  (1200mAh 603450, protected). Sense divider 100k/100k (C25803) + 100nF at GPIO1,
  tapped at BAT+ before the load-share FET.

Power budget (datasheet-derived, verify on first article): deep sleep ~65uA (S3 10-15 +
SX1262 2.3 + EPD 1 + LDO 8 + divider 21 + DW01A 3); idle LoRa RX 35-55mA; LoRa TX
155-165mA burst; WiFi 100-150mA avg / 350-500mA peaks; EPD refresh ~25mA peaks 2-4s.
Worst concurrent (WiFi peak + LoRa TX + EPD + buzzer 90mA + vibra 80mA) ~870mA, inside
the 1A LDO; firmware should stagger buzzer/vibra vs WiFi bursts below ~3.6V cell.
Sleep-life upgrade path: 1M/1M divider (ratio unchanged) if 21uA matters later.

### HMI / alerts

- 4x XKB TS-1187A-B-A-B tacts (C318884, basic): BOOT(GPIO0), UP(21), DOWN(47)
  to GND; RESET to EN. Datasheet-verified: these are TOP-push (2.0mm round plunger
  centered on the 5.1x5.1x1.5mm body, 0.25mm vertical travel), NOT side-push.
  BOOT/UP/DOWN mount on the front face below the display and actuate through
  plunger buttons in the front cover (authentic 90s pager style); RESET actuates
  through a pinhole. Contact grouping confirmed: {1,2} vs {3,4}.
- EN: 10k (C25804) to 3V3 + 1uF (C15849) to GND.
- Buzzer: MLT-8530 (C94599, ext): 3V3 to buzzer to AO3400A (C20917) drain; gate from
  GPIO15 via 100R (C22775), 10k gate pulldown; SS14 (C2480) flyback across coil.
  Magnetic buzzer: flyback is mandatory.
- Vibra: JST-ZH 2-pin B2B-ZR-SM4 (C265284) to 10mm 1027-class coin motor (hand-installed,
  buy with pre-crimped ZH leads); AO3400A + 100R + 10k pulldown on GPIO16; SS14 across
  motor terminals.
- LED: KT-0603R (C2286) + 1k on GPIO48.
- I2C debug: JST-ZH 4-pin (C265083; DNP option with 2.54mm TH fallback footprint),
  3V3/GND/SDA/SCL, **10k pullups always fitted** (no display on this bus to provide them).

### GPS: ATGM336H-5N31 (C90770, extended, ~$2.92) [added 2026-07-10 per user]

CASIC AT6558-based GPS+BDS module, LCC-18 10.1x9.7x2.4mm, internal SAW+LNA+TCXO,
default NMEA0183 @9600 8N1 (firmware autobaud probe works unchanged). <25mA
acquiring/tracking, TTFF cold <=35s / hot <=1s (needs VBAT), MSL-4, JLC-assemblable.
Datasheet: scratchpad copy atgm336h.pdf; pinout from pages 9-10 (pin 1 bottom-right,
CCW, top view): 1 GND, 2 TXD, 3 RXD, 4 1PPS (NC), 5 ON/OFF, 6 VBAT, 7 NC, 8 VCC,
9 nRESET (float), 10 GND, 11 RF_IN, 12 GND, 13 NC, 14 VCC_RF, 15 rsvd (NC), 16 SDA
(NC), 17 SCL (NC), 18 rsvd (NC).

- Power gate (true zero-leak duty cycling, NOT the ON/OFF pin): 3V3 -> AO3401A
  (C15127) source, drain -> GNSS_VCC (pin 8, 10uF C15850 + 100nF C14663 at pin);
  gate has 100k (C25803) pullup to 3V3 (default OFF) and is driven by GPIO38
  (GNSS_EN, LOW = ON). Module tolerates hard VCC cycling; no sequencing needs.
- ON/OFF (pin 5): 100k pullup to the SWITCHED GNSS_VCC (never raw 3V3, never GPIO).
- VBAT (pin 6): always-on 3V3 -> B5819W (C8598) -> VBAT with 4.7uF (C1779);
  10uA keep-alive buys the <=1s hot start (deep-sleep floor ~65uA -> ~75uA).
- UART: module TXD (pin 2) -> GPIO36 (GNSS_RX); module RXD (pin 3) <- GPIO35
  (GNSS_TX). 3.3V TTL confirmed (Vih 2.31V). GPIO35-37 were the N8R8 reserve;
  GPS outranks that option, N8R8 drop-in is hereby dropped. JTAG 39-42 and
  UART0 43/44 remain free.
- Antenna: ACTIVE 1575.42MHz antenna, off-board, u.FL #2 (second BWIPX-1-001E
  C496552). Bias tee: VCC_RF (pin 14) -> 47nH LQW15AN47NG00D (C22334, 0402
  wirewound) -> RF node; RF_IN (pin 11) -> RF node -> u.FL, 50-ohm CPW, <5mm,
  stitched ground (same rules as LoRa). Module reports antenna OPEN/SHORT via
  $GPTXT. Buy: 12mm 3.3V active ceramic patch antenna with an MHF1 pigtail (e.g. a
  12x12x4mm active GPS patch, widely available; Taoglas AA.162 class). The 12mm size
  is what the enclosure GPS pocket is sized for (was 15mm; shrunk to clear the e-paper
  module). The Molex 105262 is the 900MHz LoRa antenna: wrong band, do not reuse.
- Firmware: BOARD_CAP_GPS, gps tx=35 rx=36 baud 9600; power-gate GPIO38 active-LOW
  handled in components/gps/gps.c pager branch. CASIC uses PCAS config commands,
  not Quectel PMTK: firmware sends none (autobaud + NMEA read only). Verify the
  NMEA parser accepts $GN/$GB talker IDs at bring-up, not just $GP.

### MCU: ESP32-S3-WROOM-1-N8R2 (C2913204, extended, ~$5)

8MB flash (matches `partitions.csv`), 2MB quad PSRAM (does NOT consume GPIO35-37;
avoid N8R8 unless keeping 35-37 free, which this map already does). N8 (C2913198) is a
drop-in alternate. Module antenna end: full copper keepout all layers or board-edge
overhang per Espressif guide (the module PCB antenna serves WiFi/BLE; LoRa has its own).
Decoupling per Espressif hardware design guide. Console: USB-Serial-JTAG
(`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` in the board's sdkconfig overlay).

## Firmware deltas (tracked as separate PRs, not this repo dir)

1. **Blocking for bring-up**: `radio_dio2_rf_switch` flag in `bramble_board_config_t`
   + one-time 0x9D at init in `components/radio/sx1262.c`. Bench A/B on Heltec V3.
2. Board profile `main/boards/bramble_pager.h` + Kconfig + `board.c` branches +
   `sdkconfig.defaults.bramble_pager` (USB-Serial-JTAG console).
3. SSD1680 e-paper driver + `BOARD_CAP_DISPLAY_EPAPER` + display-pins struct variant
   (cs/dc/rst/busy). BUSY is active-high, opposite of UC8151.
4. Later: buzzer/vibra/LED alert support; battery-aware interlocks; pager duty cycle
   using `bramble.sleep` ext0 wake-on-DIO1.

## Do-not-regress invariants

- Every non-stdlib pinout and every pin-numbering orientation must be verified against
  the manufacturer datasheet (page/figure cited in COMPONENTS.md). EasyEDA part data,
  vendor symbol drawings, and "it is usually like this" do NOT count as verification.
  Claims that could not be datasheet-confirmed are tracked as open risks, not assumed.
- CC1 and CC2 each get their own 5.1k, never shared.
- Battery sense divider taps BAT+ (cell side), not VSYS.
- RESE resistor is 2.2 ohm 1% 0805 with kelvin sense to panel pin 3.
- EPD connector netlist is mirrored (connector N = panel 25-N); paper-mockup gate
  before gerbers.
- Parts under the panel deck rect stay <=3.2mm tall; the deck carries the glass 6mm
  above the PCB (module underside 4.95mm up, clearing the mated LoRa u.FL). The
  enclosure module-pocket interference check (SCAD asserts + a read of the rendered
  previews) is the physical gate, superseding the old 1mm glass keepout.
- RF trace: CPW, <5mm, stitched ground, no crossings; u.FL at case-wall edge.
- WROOM antenna keepout all layers.
- TCXO voltage 2.7V for this module (not 1.7V).
- 0x9D firmware flag lands + is bench-verified before boards are ordered.
- Netclass patterns audited against hierarchical net names (`/sheet/NET`) before fab.
- Gates: `kicad-cli sch erc --severity-error --exit-code-violations` = 0;
  `kicad-cli pcb drc --refill-zones` = 0/0/0 (never MCP ERC/DRC).

## Mechanical

Board 96x50mm landscape (GDEY0213B74 module is 59.2x29.2mm, wider than a 50mm
portrait board allows; pager is held landscape like a Motorola Advisor). 2-layer
1.6mm. The panel sits on a case-provided 6mm deck spanning board x 122..181.2,
y 51..80.2 (marked on Cmts.User): parts under the deck must be <=3.2mm tall
(excludes JST-PH battery connector and anything needing top access). The deck was
raised 4mm -> 6mm so the mated LoRa u.FL (ANT1, under the panel) clears the module
underside (4.95mm of headroom); its coax exits laterally through a deck relief channel
to the LoRa flex on the top wall. The WROOM module
pads start ~4mm inside the LEFT board edge; the module's PCB-antenna zone sits on-board
over an all-layer copper keepout (it does not overhang the edge), so the case gives it a
left-wall relief pocket. Verify no copper pour under the antenna zone before fab. USB-C
centered on the bottom edge (case datum). GPS strip reserved at board right (x 183..196).
Enclosure ~100.8 x 54.8 x 18.9mm (real computed dims from the SCAD echoes; thickness
grew with the 6mm deck, still <=19mm), OpenSCAD, printed PETG/PLA:
panel window, front-face plunger buttons over the top-push TS-1187A switches
(BOOT/UP/DOWN below the display; RESET via pinhole), antenna channel on inner wall,
battery bay (34x62x6mm cell), vibra motor pocket, snap or screw lid.
Belt clip optional but morally required.

USB-C port integration (user requirement: port lives cleanly in the body):
- The onboard receptacle IS the body port: flush-mounted through a precise case
  opening, phone-style. No panel-mount pigtail (rejected: cost, volume, 6-conductor
  CC-passing link needed for C-to-C charging + native USB, and a worse look).
- The connector is the PCB-to-case datum: USB-C centered on the bottom board edge,
  connector tongue overhanging or flush with board edge per footprint; board sits on
  fixed standoffs; the SCAD model derives the port opening from connector datasheet
  geometry (8.94mm wide x 3.26mm tall shell for TYPE-C-31-M-12) + 0.3mm clearance,
  chamfered outward so a cable boot seats cleanly.
- Case wall thickness at the port <= connector shell protrusion so plugs fully seat;
  recess the wall locally if needed.

## BOM cost (qty 1, parts only)

~$35: module $6.5 + WROOM $5 + panel $6 + antenna $5 + cell $10 + connectors/passives
~$2.5. Extended-part loading fees ~$21 across ~7 extended lines; the rest is basic tier.
