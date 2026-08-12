# Bramble Pager v1 -- Net Topology

Why every net that matters exists and how each block is wired. This covers the nets that
carry real current, RF, or a safety property, plus the strap/gate nets that gate a block
on or off. Per-pin decoupling caps are catalogued in `COMPONENTS.md` and not repeated
here. Net names are the schematic labels; where the DRU or a hard rule keys on the
hierarchical form it is written `/sheet/NET`.

The schematic (`kicad/*.kicad_sch`) is authoritative. Numbers (currents, values, part
choices) are stated in `DESIGN.md`; this file explains the wiring and cites `DESIGN.md`
sections rather than duplicating the figures.

---

## VBUS chain (USB-C input)

`VBUS` enters on USBC1 pins A4/A9/B4/B9 (both power contacts of each row tied) and is 5V
only, never PD (DESIGN "Power"). The shield and A1/A12/B1/B12 land on `GND`. VBUS fans
out to: U101 (ESD clamp), the TP4056 VCC (pins 4/8), the two charge-LED series resistors
R104/R105, the load-share gate reference R107, the Q101 load-share gate, and the D101 OR
diode anode. C101 is the VBUS input bulk.

### USB D+/D- (tie topology)

The connector presents each data line on two pads (A6/B6 = D+, A7/B7 = D-) that must be
tied together; because the two rows are interleaved these ties cross layers, so the join
is made with a short trace + via pair, not a single-layer jumper (DESIGN "Power",
USB-C). The raw pair (`USB_DP_RAW`, `USB_DM_RAW`) runs first to U101 USBLC6-2SC6 (pins
1/3, connector side) and comes out clamped as `USB_DP`/`USB_DM` (pins 4/6, MCU side) to
U401 GPIO20/19. U101 sits within 5mm of the connector. The 90-ohm differential pair
routing from U401 to U101 to the connector is a locked route.

### CC1 / CC2

Each Configuration Channel gets its own 5.1k Rd pulldown to GND: `CC1` through R101,
`CC2` through R102. They are never shared (do-not-regress invariant). This advertises a
sink to a 5V-only source and passes CC through for a C-to-C cable.

## Battery, protection, and BAT_MINUS

The cell connects at BATT1: pin 1 is `BAT_PLUS`, pin 2 is `BAT_MINUS`. `BAT_MINUS` is the
raw cell negative and is deliberately NOT board `GND`. The DW01A + FS8205A protection
pair sits in the cell-negative leg between `BAT_MINUS` and `GND`:

- U103 DW01A monitors cell voltage (VCC sensed at `BAT_PLUS` through R106 100R, with
  C106 100nF to `BAT_MINUS`, the DW01A app-note RC) and current (across the FET stack).
- U104 FS8205A is two back-to-back N-FETs. Using its NONSTANDARD pinout (1=S1, 2=D,
  3=S2, 4=G2, 5=D, 6=G1, confirmed against the netlist), S1 (pin 1) = `BAT_MINUS`, S2
  (pin 3) = board `GND`, the drains (pins 2/5) tie together as the mid-node, gate G1
  (pin 6) = DW01A OD, gate G2 (pin 4) = DW01A OC.
- On overcharge, overdischarge, or overcurrent the DW01A pulls a gate low and opens the
  path between `BAT_MINUS` and `GND`, disconnecting the cell. Kept even for protected
  cells (DESIGN "Power", Protection).

## Load-share (AN1149 topology)

The AN1149 / Adafruit power-path lets the board charge while running and terminate
cleanly. `BAT_PLUS` feeds the source-drain of Q101 AO3401A P-FET (drain = `BAT_PLUS`,
source = `VSYS`); the gate is referenced to `VBUS` with R107 100k. In parallel, D101 SS34
ORs `VBUS` into `VSYS` (anode VBUS, cathode VSYS).

- USB present: `VBUS` = 5V pulls the Q101 gate high, the P-FET turns off, the cell is
  isolated, and the load runs from `VBUS` through D101 (`VSYS` ~= 4.6V). Charge current
  into the cell is unloaded, so TP4056 termination is accurate.
- USB absent: the gate is pulled low relative to source, Q101 conducts, and the cell
  feeds `VSYS` directly (DESIGN "Power", Load share).

## VSYS and +3V3

`VSYS` (load-share output) is the LDO input. U105 XC6220B331MR-G takes `VSYS` on VIN
(pin 1), with CE (pin 3) tied to VIN so it never floats, and produces `+3V3` on VOUT
(pin 5). C104 is the VSYS input bulk, C105 the 3V3 output bulk. `+3V3` is the board rail
for the WROOM, the LoRa module VCC, the GNSS gate source, the GNSS VBAT keep-alive, the
EPD boost, the alert-coil highs, and the I2C pullups.

`+3V3` is in the `power_rail_min_width` DRU rule (0.4mm floor) alongside `VBUS`, `VSYS`,
`BAT_PLUS`, `BAT_MINUS`, because worst-case concurrent draw is ~870mA (DESIGN power
budget).

## Battery sense divider

`VBAT_SENSE` is the fuel-gauge tap into U401 GPIO1 (ADC1_CH0). R108 100k (top) taps
`BAT_PLUS` on the cell side, before the load-share FET, per the do-not-regress invariant;
R109 100k (bottom) to GND; C107 100nF filters at the pin. Divider factor 2, matching
firmware `divider_factor=2`. Sensing at `BAT_PLUS` (not `VSYS`) means the reading tracks
true cell voltage regardless of USB presence. The ratio is unchanged if the divider is
later upsized to 1M/1M for a lower sleep draw (DESIGN "Power", sleep-life upgrade).

## LoRa RF (`/radio/RF_ANT`)

U201 pin 9 (ANT) drives `RF_ANT` straight to ANT1 (u.FL). This is the hardest rule on the
board: coplanar waveguide, run kept under 5mm, ~1.0-1.2mm trace with 0.5mm gap to the top
pour, solid L2 ground beneath, stitching vias every 2-3mm, nothing crossing underneath
(DESIGN "RF path"). The DRU enforces the non-negotiable parts: `rf_ant_no_vias` (no vias
on the net), `rf_ant_front_only` (F.Cu tracks only), `rf_ant_min_width` (>=1.0mm). ANT1
sits at the board edge nearest the case wall carrying the antenna. The <5mm length is
enforced by review, not the DRU.

The module's RF switch is chip-internal on DIO2 and not exposed; DIO2 has no net.
Firmware must issue `SetDio2AsRfSwitchCtrl` (0x9D, payload 0x01) once at init or TX is
dead. DIO3 (pin 11) is left unconnected: the module powers its own TCXO from DIO3
internally (DESIGN "Radio"). Both facts are netlist-confirmed: pin 11 has no connection.

## GNSS RF and bias tee (`/radio/GNSS_RF`)

The ATGM336H uses an active antenna, so the RF node carries both the 1575.42MHz signal
and the DC bias. `GNSS_VCCRF` (module VCC_RF, pin 14) feeds through L201 47nH wirewound
onto the RF node; RF_IN (module pin 11) and ANT2 (u.FL) join that same node. The
inductor passes DC bias up to the antenna while blocking RF from the supply; the antenna
element and the module's internal SAW see the signal. Same RF discipline as LoRa: the DRU
enforces `gnss_rf_no_vias`, `gnss_rf_front_only`, `gnss_rf_min_width` (>=1.0mm), and the
<5mm CPW rule is review-enforced (DESIGN "GPS", Antenna). The module reports antenna
OPEN/SHORT over `$GPTXT`.

## GNSS_VCC gate (P-FET power gate)

True zero-leak duty cycling, done by hard-cycling VCC rather than the module's ON/OFF
pin. `+3V3` feeds Q201 AO3401A source (pin 2); the drain (pin 3) is `GNSS_VCC` to module
VCC (pin 8), with C204 10uF + C205 100nF at the pin. The gate is `GNSS_EN`, pulled to
`+3V3` by R201 100k so the default state is OFF, and driven by U401 GPIO38: LOW = GNSS
on. The module tolerates hard VCC cycling with no sequencing needs (DESIGN "GPS", Power
gate).

The module ON/OFF pin (5) sits on `GNSS_ONOFF`, pulled up through R202 100k to the
SWITCHED `GNSS_VCC`, never to raw 3V3 and never to a GPIO. When VCC is gated off the
pullup collapses with it, so ON/OFF cannot back-bias the powered-down module.

## GNSS_VBAT keep-alive

Independent of the VCC gate, `GNSS_VBAT` (module pin 6) is held up from always-on `+3V3`
through D201 B5819W (anode GNSS_VBAT, cathode +3V3) with C203 4.7uF. This ~10uA
keep-alive preserves the RTC and ephemeris so a gated-off module still gets a <=1s hot
start; it lifts the deep-sleep floor only marginally (DESIGN "GPS", VBAT).

## EPD boost and negative pump

The panel needs several bias rails generated on-sheet from `+3V3`. The switch node
`EPD_SW` is the shared node of the boost inductor and the switch FET:

- Boost: `+3V3` through L301 47uH into `EPD_SW`; Q301 AO3400A drain = `EPD_SW`, source
  through R302 to GND. The panel's own gate driver output GDR (`EPD_GDR`) switches Q301,
  with R301 1M holding the gate off when idle. D303 B5819W rectifies `EPD_SW` up to
  `EPD_VGH` (cathode on VGH). `EPD_SW` is in the DRU `epd_sw_min_width` rule (0.4mm) and
  its loop must stay tight (DESIGN "Display").
- Negative pump: C302 4.7uF couples `EPD_SW` into a flying node; D302 clamps that node
  against GND on one half-cycle and D301 delivers the pumped-negative to `EPD_VGL` on the
  other. This makes the negative gate rail without a separate inductor.
- Each panel rail (`EPD_VGH/VGL/VSH1/VSH2/VSL/VCOM/VDD` and VCI on `+3V3`) has a 1uF
  reservoir at the connector (C303-C310).

### RESE kelvin sense

R302 2.2R 1% is the RESE current-sense resistor for THIS panel (2.2 ohm, not the 0.47
ohm of other GoodDisplay refs). Q301's source lands on the R302 top node, and that node
is the RESE net routed back to panel pin 3 (`EPD_RESE`, connector pin 22) as a kelvin
sense (do-not-regress invariant). Value and 1% tolerance are critical.

### EPD connector mirror

The 0.3mm panel tail folds 180 degrees under the panel into the FH34SRJ-24S, so the pin
order MIRRORS: connector pin N = panel pin 25-N (DESIGN "Display"). The schematic is wired
to that mirror, netlist-confirmed at the corners: EPD1 pin 1 = `EPD_VCOM` = panel pin 24,
EPD1 pin 13 = `EPD_CS` = panel pin 12, EPD1 pin 22 = `EPD_RESE` = panel pin 3. BS1 is tied
low (4-wire SPI) and `EPD_BUSY` is active-HIGH (SSD1680; firmware waits for low). The 1:1
paper-mockup gate before gerbers exists precisely because this fold geometry has to be
physically right, not just electrically consistent.

`EPD_SW` also carries a hard rule of its own beyond width: keep the boost loop tight and
under the panel-glass keepout (nothing taller than ~1mm under the glass footprint).

## EN / BOOT straps

`EN` (WROOM pin 3, chip reset) has R401 10k pullup to `+3V3` and C403 1uF to GND for a
clean power-on reset. SW401 (RESET) shorts `EN` to GND through a case pinhole. `BOOT` is
GPIO0, the strapping-canonical boot select, pulled to GND by SW402 (front-face plunger).
DOWN (`BTN_DOWN`, GPIO21, SW403 middle front plunger) and UP (`BTN_UP`, GPIO47, SW404
rightmost plunger; BOOT/SW402 is the leftmost of the three) use internal pullups and their tacts short to GND. DOWN sits on GPIO21
deliberately: only RTC GPIOs 0-21 can wake the ESP32-S3 from deep sleep and GPIO47 has
no RTC alias, so the primary scroll/wake button gets the RTC-capable pin (rev B pre-fab
swap; UP cannot deep-sleep-wake, accepted trade). All four TS-1187A tacts are top-push,
contacts grouped {1,2} vs {3,4} (DESIGN "HMI").

## Buzzer / vibra low-side drivers

Both alerts are high-side coil, low-side switch, with a flyback across the coil:

- Buzzer: `+3V3` to BZ1 MLT-8530 pin 1, coil to Q401 AO3400A drain (pin 3). GPIO15 drives
  `BUZ_GATE` through R402 100R to the gate, with R404 10k gate pulldown. D401 SS14 flyback
  across the coil (cathode `+3V3`). Flyback is mandatory for the magnetic buzzer. Firmware
  drives it with LEDC PWM ~2.7kHz.
- Vibra: `+3V3` to VIB1 pin 1, motor to Q402 AO3400A drain (pin 3). GPIO16 drives
  `VIB_GATE` through R403 100R, R405 10k pulldown, D402 SS14 flyback across the motor
  (DESIGN "HMI"). Firmware should stagger buzzer/vibra against WiFi/LoRa bursts below
  ~3.6V cell (DESIGN power budget).

## I2C and status LED

`I2C_SDA`/`I2C_SCL` (GPIO17/18) go to the JST-ZH debug header I2C1 with R407/R408 10k
pullups always fitted (nothing else on the bus provides them; do-not-regress). `LED_R`
(GPIO48) drives LED401 through R406 1k, active high.
