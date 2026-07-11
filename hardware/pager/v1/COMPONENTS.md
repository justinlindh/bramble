# Bramble Pager v1 -- Component Reference

Every component in the schematic: what it is, what it connects to, why it exists, and the
datasheet citation that confirms its pinout.

**Source of truth:** the KiCad schematic sheets in `kicad/` (`power`, `radio`, `epaper`,
`mcu`; the root `pager.kicad_sch` holds only the hierarchical references). This document
mirrors them; if it disagrees with a sheet, the sheet wins. Nets are given by the
schematic label names; internal nets with no label are described by role.

82 components across four sheets. Every non-stdlib pinout below was verified against the
manufacturer datasheet, per the `DESIGN.md` do-not-regress invariant. Passives carry no
pinout note (symmetric parts). Reference cells: `.pdf` datasheet copies live in the
scratchpad / EasyEDA import set noted in `DESIGN.md`.

---

## Power sheet (`/power/`)

USB-C input, TP4056 charger, DW01A + FS8205A cell protection, AN1149-style load-share,
and the 3.3V LDO. See `NET_TOPOLOGY.md` for the full power chain.

| Ref | Part | Package | LCSC | Nets | Purpose | Pinout note |
|---|---|---|---|---|---|---|
| USBC1 | TYPE-C-31-M-12 USB-C receptacle | HRO TYPE-C-31-M-12 SMD | C165948 | VBUS (A4/A9/B4/B9), GND (A1/A12/B1/B12/SH), CC1 (A5), CC2 (B5), USB_DP_RAW (A6/B6), USB_DM_RAW (A7/B7) | Body port and PCB-to-case datum. 5V VBUS only, never PD. D+ pairs and D- pairs tied. Shield to GND. | USB2 receptacle; A6/B6 = DP, A7/B7 = DM interleaved (require layer-change ties, see NET_TOPOLOGY). |
| U101 | USBLC6-2SC6 ESD array | SOT-23-6 | C2687116 | VBUS (5), GND (2), USB_DP_RAW (1), USB_DP (4), USB_DM_RAW (3), USB_DM (6) | TVS ESD protection on VBUS + both data lines. Placed within 5mm of USBC1. | ST datasheet; genuine ST is C7519, C2687116 is the fitted line. Raw on 1/3, MCU-side clamps on 4/6. |
| R101 | 5.1k 1% | 0603 | C23186 | CC1, GND | CC1 pulldown (Rd). Sinks 5V-only source detection. | -- |
| R102 | 5.1k 1% | 0603 | C23186 | CC2, GND | CC2 pulldown (Rd). Its own resistor, never shared with CC1. | -- |
| U102 | TP4056-42 Li-ion charger | ESOP-8 (EP) | C16581 | VBUS (VCC 4/8), BAT_PLUS (BAT 5), PROG (2), CHRG_LED (7), STDBY_LED (6), GND (1/3/9), TEMP to GND | 1S LiPo linear charger, 500mA set by PROG. CE tied to VCC (always enabled). EP to GND pour + thermal vias (~1W worst case, keep off radio/TCXO). | TopPower REV_2.4 pin table p.4/7-8. PROG constant 1100 (not 1200); RPROG 2.2k = 500mA per EC table p.5. EP solder to GND p.13. |
| C101 | 10uF 25V X5R | 0805 | C15850 | VBUS, GND | TP4056 VCC input bulk. | -- |
| C102 | 10uF 25V X5R | 0805 | C15850 | BAT_PLUS, GND | TP4056 BAT-side bulk. | -- |
| C104 | 10uF 25V X5R | 0805 | C15850 | VSYS, GND | LDO input bulk (U105 VIN). | -- |
| C105 | 10uF 25V X5R | 0805 | C15850 | +3V3, GND | LDO output bulk (U105 VOUT). | -- |
| C106 | 100nF 50V X7R | 0603 | C14663 | DW01A VCC node, BAT_MINUS | DW01A VCC decoupling; forms the 100R+100nF RC with R106 per DW01A app circuit. | -- |
| C107 | 100nF 50V X7R | 0603 | C14663 | VBAT_SENSE, GND | Battery-sense divider filter cap at the GPIO1 tap. | -- |
| R103 | 2.2k 1% | 0603 | C4190 | PROG, GND | TP4056 charge-current set. 2.2k = 500mA (see U102 note). | -- |
| R104 | 1k | 0603 | C21190 | VBUS, CHRG_LED | Series limit for the red CHRG LED. | -- |
| R105 | 1k | 0603 | C21190 | VBUS, STDBY_LED | Series limit for the green STDBY LED. | -- |
| R106 | 100R | 0603 | C22775 | BAT_PLUS, DW01A VCC node | DW01A VCC series resistor (100R half of the DW01A app RC). | -- |
| R107 | 100k | 0603 | C25803 | VBUS, GND | Load-share gate pulldown: holds Q101 gate at VBUS reference; see NET_TOPOLOGY. | -- |
| R108 | 100k | 0603 | C25803 | BAT_PLUS, VBAT_SENSE | Battery-sense divider top leg. Taps BAT_PLUS (cell side), factor 2. | -- |
| R109 | 100k | 0603 | C25803 | VBAT_SENSE, GND | Battery-sense divider bottom leg. Ratio unchanged if upgraded to 1M/1M. | -- |
| D101 | SS34 Schottky | SMA | C8678 | VSYS (K), VBUS (A) | Load-share OR diode: passes VBUS to VSYS when USB present so termination is clean. | SMA cathode band = VSYS. |
| Q101 | AO3401A P-channel MOSFET | SOT-23 | C15127 | VBUS (gate 1), VSYS (source 2), BAT_PLUS (drain 3) | Load-share pass FET. USB present pulls gate high (VBUS): FET off, cell isolated, load runs from VBUS. USB absent: gate low, cell feeds VSYS. | AO3401A SOT-23: 1 G, 2 S, 3 D. |
| U103 | DW01A cell protection IC | SOT-23-6 | C351410 | DW01_OD (1), GND (2/CS current sense), DW01_OC (3), TD NC (4), DW01A VCC node (5/VCC), BAT_MINUS (6/VSS) | Overcharge / overdischarge / overcurrent monitor. Drives the FS8205A gates. Kept even with protected cells. | DW01A SOT-23-6 standard pinout. |
| U104 | FS8205A dual N-MOSFET | SOT-23-6 | C2830320 | BAT_MINUS (S1 1), shared drain (D 2/5), GND (S2 3), DW01_OC (G2 4), DW01_OD (G1 6) | Back-to-back protection FETs between cell- and pack GND. | TECH PUBLIC datasheet p.1: NONSTANDARD pinout 1=S1 2=D 3=S2 4=G2 5=D 6=G1 (differs from classic Fortune 8205A). Netlist-confirmed: S1=BAT_MINUS, S2=GND, G1=DW01 OD, G2=DW01 OC. |
| U105 | XC6220B331MR-G LDO | SOT-23-5 | C86534 | VSYS (VIN 1, CE 3), GND (VSS 2), +3V3 (VOUT 5), NC (4) | 3.3V 1A LDO, ~100mV dropout @500mA, 8uA IQ. CE tied to VIN (never floats). Genuine Torex only. | Torex pin assignment p.2: 1 VIN 2 VSS 3 CE 4 NC 5 VOUT. |
| LED101 | Red LED (CHRG) | 0603 | C2286 | CHRG_LED, GND | Charge-in-progress indicator, from VBUS via R104. | -- |
| LED102 | Green LED (STDBY) | 0603 | C965805 | STDBY_LED, GND | Charge-done indicator, from VBUS via R105. | -- |
| BATT1 | JST-PH S2B-PH-SM4-TB 2-pin | JST PH 2.0mm horizontal SMD | C295747 | BAT_PLUS (1), BAT_MINUS (2) | 1S LiPo cell connector. Pin 1 = BAT+ (Adafruit convention), prominent silkscreen. DMM polarity check every cell before first plug. | -- |

## Radio sheet (`/radio/`)

LoRa transceiver, GNSS receiver, and both u.FL RF feeds with the GNSS bias tee and power
gate. See `NET_TOPOLOGY.md` for the RF and gate topology.

| Ref | Part | Package | LCSC | Nets | Purpose | Pinout note |
|---|---|---|---|---|---|---|
| U201 | NiceRF LoRa1262-915TCXO (SX1262 module) | 16-pin castellated 16x16mm, shielded | C5356643 | GND (1/8/10), SPI_MISO (2), SPI_MOSI (3), SPI_SCK (4), RADIO_NSS (5), RADIO_NRESET (6), ANT/RF_ANT (9), DIO3 NC (11), +3V3 VCC (13), RADIO_DIO1 (15), RADIO_BUSY (16); pins 7/12/14 NC | US915 LoRa transceiver. FCC ID 2AD6-1262. VCC 10uF+100nF at pin 13. Regulator DC-DC, `radio_osc = TCXO_DIO3`. | NiceRF LoRa126X datasheet Rev 2.1 Sec 7 p.7-8, CCW ring. DIO3 (11) left NC: module powers its TCXO from DIO3 internally (Chinese-edition p.8 note). RF switch is chip-internal on DIO2 (Sec 1 + Sec 9): firmware must send SetDio2AsRfSwitchCtrl or TX is dead. TCXO 2.8V default so SX1262 code 2.7V (Semtech Table 13-35). Netlist-confirmed: pin 11 unconnected. |
| C201 | 10uF 25V X5R | 0805 | C15850 | +3V3, GND | U201 VCC bulk at pin 13. | -- |
| C202 | 100nF 50V X7R | 0603 | C14663 | +3V3, GND | U201 VCC HF bypass at pin 13. | -- |
| U202 | ATGM336H-5N31 GNSS module (AT6558, GPS+BDS) | LCC-18 10.1x9.7x2.4mm | C90770 | GND (1/10/12), GNSS_RX (TXD 2), GNSS_TX (RXD 3), 1PPS NC (4), GNSS_ONOFF (ON/OFF 5), GNSS_VBAT (6), GNSS_VCC (VCC 8), nRESET float (9), RF_IN/GNSS_RF (11), GNSS_VCCRF (VCC_RF 14); 7/13/15-18 NC | Internal SAW+LNA+TCXO, NMEA0183 @9600 8N1. Hard VCC cycling tolerated (power-gated by Q201). | LCC-18 pinout p.9-10, land pattern p.8-9 (pin 1 bottom-right, CCW, top view). VBAT 10uA keep-alive p.11, TTFF p.12. Active-antenna bias off VCC_RF p.13-14. Module TXD (2) drives ESP RX; module RXD (3) driven by ESP TX. |
| Q201 | AO3401A P-channel MOSFET | SOT-23 | C15127 | GNSS_EN (gate 1), +3V3 (source 2), GNSS_VCC (drain 3) | GNSS power gate for zero-leak duty cycling. GPIO38 LOW = GNSS on. | AO3401A SOT-23: 1 G, 2 S, 3 D. |
| R201 | 100k | 0603 | C25803 | +3V3, GNSS_EN | Q201 gate pullup: default OFF (gate at 3V3). | -- |
| R202 | 100k | 0603 | C25803 | GNSS_VCC, GNSS_ONOFF | ON/OFF (pin 5) pullup to the SWITCHED GNSS_VCC, never raw 3V3, never a GPIO. | -- |
| C204 | 10uF 25V X5R | 0805 | C15850 | GNSS_VCC, GND | GNSS_VCC bulk at module pin 8. | -- |
| C205 | 100nF 50V X7R | 0603 | C14663 | GNSS_VCC, GND | GNSS_VCC HF bypass at module pin 8. | -- |
| D201 | B5819W Schottky | SOD-123 | C8598 | +3V3 (A), GNSS_VBAT (K) | VBAT keep-alive diode: always-on 3V3 to VBAT (pin 6), 10uA keep-alive for hot start. | Anode = +3V3, cathode = GNSS_VBAT; SOD-123 cathode band faces GNSS_VBAT. (Schematic netlist is correct; earlier doc text had it reversed.) |
| C203 | 4.7uF 25V X5R | 0805 | C1779 | GNSS_VBAT, GND | VBAT hold-up cap for the RTC/ephemeris keep-alive. | -- |
| L201 | 47nH wirewound (LQW15AN47NG00D) | 0402 | C22334 | GNSS_RF node, GNSS_VCCRF | Bias-tee inductor: feeds VCC_RF onto the RF node to bias the active GNSS antenna. | -- |
| ANT1 | u.FL BWIPX-1-001E | Hirose U.FL-R-SMT-1 vertical | C496552 | RF_ANT (1), GND (2) | LoRa antenna connector. At the case-wall board edge; <5mm CPW from U201 pin 9. | -- |
| ANT2 | u.FL BWIPX-1-001E | Hirose U.FL-R-SMT-1 vertical | C496552 | GNSS_RF node (1), GND (2) | GNSS antenna connector. Active 1575.42MHz antenna, 50-ohm CPW, same RF rules as ANT1. | -- |

## E-paper sheet (`/epaper/`)

Display connector plus its boost / negative charge-pump. The connector netlist is
mirrored: connector pin N = panel pin 25-N (fold-under mount). See `NET_TOPOLOGY.md`.

| Ref | Part | Package | LCSC | Nets | Purpose | Pinout note |
|---|---|---|---|---|---|---|
| EPD1 | Hirose FH34SRJ-24S-0.5SH FPC connector (mates GDEY0213B74 panel tail) | 0.5mm 24-pin dual-contact SMD | C324726 | VCOM (1), VGL (2), VSL (3), VGH (4), VSH1 (5), VDD (7), GND (8/17), +3V3 VCI (9/10), SPI_MOSI (11), SPI_SCK (12), EPD_CS (13), EPD_DC (14), EPD_RST (15), EPD_BUSY (16), VSH2 (20), RESE (22), GDR (23); 6/18/19/21/24 open per panel | SSD1680 2.13" 250x122 panel interface. 0.3mm panel tail, 180-degree fold. | GDEY0213B74 panel pin table p.8. BUSY active-HIGH p.9 note 5-4; BS1=L for 4-wire SPI note 5-5; RESE 2.2R ref circuit p.29; connector-to-panel mirror (connector N = panel 25-N) with fold-under mount, mech p.7. Netlist-confirmed mirror: EPD1.1=VCOM=panel24, EPD1.13=CS=panel12, EPD1.22=RESE=panel3. |
| L301 | 47uH (FNR4030S470MT) | FNR4030 4x4mm SMD | C167888 | +3V3, EPD_SW node | Boost inductor: 3V3 to switch node. | -- |
| Q301 | AO3400A N-channel MOSFET | SOT-23 | C20917 | EPD_GDR (gate 1), R302 source node (source 2), EPD_SW node (drain 3) | Boost switch driven by panel GDR. Source through R302 (RESE) to GND. | AO3400A SOT-23: 1 G, 2 S, 3 D. |
| R301 | 1M | 0603 | C22935 | EPD_GDR, GND | GDR gate pulldown: holds Q301 off when the panel is not driving. | -- |
| R302 | 2.2R 1% | 0805 | C2933402 | Q301 source node, GND | RESE current-sense resistor. 2.2 ohm for THIS panel (not 0.47). Kelvin-sensed at the source node to panel pin 3 (RESE). | -- |
| C301 | 4.7uF 25V X5R | 0805 | C1779 | +3V3, GND | Boost input reservoir at the L301 feed. | -- |
| C302 | 4.7uF 25V X5R | 0805 | C1779 | EPD_SW node, flying node | Negative-pump flying cap: couples the switch node into the D301/D302 pump. | -- |
| D301 | B5819W Schottky | SOD-123 | C8598 | flying node (A), EPD_VGL (K) | Negative-pump output diode: delivers the pumped negative rail to VGL. | -- |
| D302 | B5819W Schottky | SOD-123 | C8598 | GND (A), flying node (K) | Negative-pump clamp diode: clamps the flying node against GND each cycle. | -- |
| D303 | B5819W Schottky | SOD-123 | C8598 | EPD_VGH (K), EPD_SW node (A) | Positive boost rectifier: switch node to VGH. | -- |
| C303 | 1uF 50V X7R | 0603 | C15849 | EPD_VSH2, GND | Panel rail reservoir at the connector (VSH2). | -- |
| C304 | 1uF 50V X7R | 0603 | C15849 | EPD_VGH, GND | Panel rail reservoir (VGH). | -- |
| C305 | 1uF 50V X7R | 0603 | C15849 | +3V3, GND | VCI reservoir at the connector (panel pins 9/10). | -- |
| C306 | 1uF 50V X7R | 0603 | C15849 | EPD_VDD, GND | Panel internal-LDO reservoir (VDD, pin 7). | -- |
| C307 | 1uF 50V X7R | 0603 | C15849 | EPD_VSH1, GND | Panel rail reservoir (VSH1). | -- |
| C308 | 1uF 50V X7R | 0603 | C15849 | EPD_VSL, GND | Panel rail reservoir (VSL). | -- |
| C309 | 1uF 50V X7R | 0603 | C15849 | EPD_VGL, GND | Panel rail reservoir (VGL). | -- |
| C310 | 1uF 50V X7R | 0603 | C15849 | EPD_VCOM, GND | Panel rail reservoir (VCOM). | -- |

## MCU sheet (`/mcu/`)

ESP32-S3, reset network, four tact buttons, buzzer and vibra low-side drivers, status
LED, and the I2C debug header.

| Ref | Part | Package | LCSC | Nets | Purpose | Pinout note |
|---|---|---|---|---|---|---|
| U401 | ESP32-S3-WROOM-1-N8R2 | RF_Module WROOM-1 | C2913204 | +3V3, GND, EN (3), plus every signal net: SPI bus (17/18/19), RADIO_NSS/NRESET/BUSY/DIO1, EPD_CS/DC/RST/BUSY, USB_DM (13)/USB_DP (14), I2C_SDA/SCL, BUZ_GATE (8)/VIB_GATE (9), GNSS_TX/RX/EN, BTN_UP/DOWN, BOOT, LED_R, VBAT_SENSE (39) | 8MB flash, 2MB quad PSRAM. Native USB console (USB-Serial-JTAG). Antenna-end copper keepout all layers. | Espressif WROOM-1 module. N8 (C2913198) is a drop-in alternate; N8R8 dropped when GPIO35/36 went to GPS. |
| C401 | 10uF 25V X5R | 0805 | C15850 | +3V3, GND | WROOM 3V3 bulk. | -- |
| C402 | 100nF 50V X7R | 0603 | C14663 | +3V3, GND | WROOM 3V3 HF bypass. | -- |
| C403 | 1uF 50V X7R | 0603 | C15849 | EN, GND | EN RC delay cap (with R401) for clean reset. | -- |
| R401 | 10k | 0603 | C25804 | +3V3, EN | EN pullup. | -- |
| SW401 | TS-1187A tact (RESET) | XKB 5.1x5.1x1.5mm top-push SMD | C318884 | EN (1/2), GND (3/4) | RESET: shorts EN to GND. Actuated through a case pinhole. | XKB drawing: TOP-push 2.0mm plunger, 0.25mm travel; contacts {1,2}|{3,4}. |
| SW402 | TS-1187A tact (BOOT) | XKB 5.1x5.1x1.5mm top-push SMD | C318884 | BOOT (1/2), GND (3/4) | BOOT/SELECT on GPIO0 (strapping-canonical), to GND. Front-face plunger. | Same as SW401. |
| SW403 | TS-1187A tact (DOWN) | XKB 5.1x5.1x1.5mm top-push SMD | C318884 | BTN_DOWN (1/2), GND (3/4) | DOWN button on GPIO21 (RTC-capable: deep-sleep wake), internal pullup. Front-face plunger. | Same as SW401. |
| SW404 | TS-1187A tact (UP) | XKB 5.1x5.1x1.5mm top-push SMD | C318884 | BTN_UP (1/2), GND (3/4) | UP button on GPIO47 (no RTC alias: cannot deep-sleep-wake), internal pullup. Front-face plunger. | Same as SW401. |
| BZ1 | MLT-8530 magnetic buzzer | 8.5x8.5mm SMD | C94599 | +3V3 (1), Q401 drain (2) | Pager alert tone. LEDC PWM ~2.7kHz through the Q401 low-side. | Huaneng p.2/p.4: + and - leads adjacent on one edge, dummies opposite, sound port on the dummy side. |
| Q401 | AO3400A N-channel MOSFET | SOT-23 | C20917 | BZ gate node (gate 1), GND (source 2), buzzer low side (drain 3) | Buzzer low-side switch from GPIO15. | AO3400A SOT-23: 1 G, 2 S, 3 D. |
| R402 | 100R | 0603 | C22775 | BUZ_GATE, Q401 gate node | Gate series resistor. | -- |
| R404 | 10k | 0603 | C25804 | Q401 gate node, GND | Buzzer gate pulldown (default off). | -- |
| D401 | SS14 Schottky | SMA | C2480 | +3V3 (K), buzzer low side (A) | Buzzer coil flyback. Mandatory for the magnetic buzzer. | SMA cathode band = +3V3. |
| VIB1 | JST-ZH B2B-ZR-SM4 2-pin (coin vibra motor) | JST ZH 1.5mm SMD | C265284 | +3V3 (1), Q402 drain (2) | Coin vibration motor, hand-installed with pre-crimped ZH leads. | -- |
| Q402 | AO3400A N-channel MOSFET | SOT-23 | C20917 | VIB gate node (gate 1), GND (source 2), motor low side (drain 3) | Vibra low-side switch from GPIO16. | AO3400A SOT-23: 1 G, 2 S, 3 D. |
| R403 | 100R | 0603 | C22775 | VIB_GATE, Q402 gate node | Gate series resistor. | -- |
| R405 | 10k | 0603 | C25804 | Q402 gate node, GND | Vibra gate pulldown (default off). | -- |
| D402 | SS14 Schottky | SMA | C2480 | +3V3 (K), motor low side (A) | Motor flyback across the coil. | SMA cathode band = +3V3. |
| LED401 | Red LED (status) | 0603 | C2286 | LED_R node (via R406), GND | Status LED on GPIO48, active high, 1k series. | -- |
| R406 | 1k | 0603 | C21190 | LED_R, LED401 anode node | Status LED current limit. | -- |
| R407 | 10k | 0603 | C25804 | +3V3, I2C_SDA | I2C SDA pullup. Always fitted (no display on this bus to provide it). | -- |
| R408 | 10k | 0603 | C25804 | +3V3, I2C_SCL | I2C SCL pullup. Always fitted. | -- |
| I2C1 | JST-ZH B4B-ZR-SM4 4-pin (I2C debug) | JST ZH 1.5mm SMD | C265083 | +3V3 (1), I2C_SDA (2), I2C_SCL (3), GND (4) | Debug I2C header. DNP option with a 2.54mm TH fallback footprint. | -- |

---

## Power rails

| Rail | Voltage | Source | Consumers | Bulk |
|---|---|---|---|---|
| VBUS | 5V | USBC1 (USB only, never PD) | U101 (ESD), U102 (charger VCC), R104/R105 (LED feeds), R107 (load-share ref), Q101 gate, D101 anode | C101 |
| BAT_PLUS | ~3.0-4.2V | BATT1 cell + | U102 (BAT), Q101 drain (load-share), R106 (DW01A VCC), R108 (sense divider top) | C102 |
| VSYS | ~3.6-4.6V | Q101 (cell) OR D101 (VBUS) | U105 (LDO VIN) | C104 |
| +3V3 | 3.3V | U105 XC6220 | U401 (WROOM), U201 (LoRa VCC), Q201 source (GNSS gate), D201 (GNSS VBAT), EPD1 VCI + L301 (EPD boost), buzzer/vibra coil highs, I2C pullups, status | C105, C201, C301, C305, C401 + per-block |
| GND | 0V | Common return | All | F.Cu + B.Cu pour |

`BAT_MINUS` is the cell negative and is NOT board GND: the FS8205A protection FETs sit
between `BAT_MINUS` and `GND`. `GNSS_VCC` is the switched (power-gated) 3V3 for the GNSS
module; `GNSS_VBAT` is the always-on keep-alive fed through D201. The EPD panel rails
(`EPD_VGH/VGL/VSH1/VSH2/VSL/VCOM/VDD`) are generated by the on-sheet boost / charge-pump,
see `NET_TOPOLOGY.md`.
