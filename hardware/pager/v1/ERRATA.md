# Bramble Pager v1 -- Errata

No errata yet (rev has not been fabricated).

Confirmed hardware defects go here once boards exist, newest first, each with: symptom,
root cause, affected units, and the workaround or fix. Until then the list below is
pre-fab open items, not errata.

---

## Pre-fab open items

These must be closed before boards are ordered. See the `README.md` bring-up checklist
and the `DESIGN.md` do-not-regress invariants.

- **FPC mockup gate pending.** Print the GDEY0213B74 panel tail + 180-degree fold +
  FH34SRJ-24S connector at 1:1 and confirm the mirrored pin order (connector pin N =
  panel pin 25-N) physically lands before generating gerbers. The entire EPD netlist
  depends on the fold geometry, and it is a hard gate. The 1:1 artwork to print is
  `mockup-fpc-1to1.pdf` in this directory (source `mockup-fpc-1to1.svg`).

- **TCXO voltage bring-up verify pending.** The LoRa1262 module's TCXO is 2.8V nominal;
  firmware sets the nearest SX1262 code, 2.7V (NOT Heltec V3's 1.7V). Confirm the radio
  brings up and TXs cleanly at 2.7V on a first article; if not, try 3.0V. Bench against a
  known-good Heltec V3 for a reference RSSI.

- **Heltec V3 fleet DIO2 RF-switch A/B test pending (Gitea issue #181).** The
  `radio_dio2_rf_switch` flag (one-time 0x9D `SetDio2AsRfSwitchCtrl` at init) is blocking
  for this board and must be bench-verified before ordering. Separately, run the A/B on a
  Heltec V3 (fixed-peer RSSI with the flag on vs off): Meshtastic sets it for V3 and
  Bramble does not, so this may be a latent fleet TX bug worth chasing regardless of the
  pager. The fleet-wide A/B is tracked as Gitea issue #181.

- **freerouting headless hangs.** The headless freerouting run hangs on this project. Use
  the KiCad GUI autorouter manually, or run the freerouting jar under a `timeout` wrapper
  so a hang does not stall the pipeline. Do not block routing on the headless path.

- **JLC assembly-tier check at order time.** Re-check the economic-vs-standard PCBA tier
  for the extended lines C5356643 (LoRa1262 module) and C2913204 (ESP32-S3-WROOM-1) when
  placing the order. Extended parts and any that force a standard-tier setup change the
  assembly cost; confirm both are placeable on the intended tier before committing.

- **Final DRC 0/0/0 + netclass audit before gerbers (review M2 + MINOR).** The review-fix
  commits added USB and RF netclasses, finished/classed the USB pair, and moved the ESD.
  Re-run `kicad-cli pcb drc --refill-zones` and confirm 0 unconnected / 0 clearance / 0
  dangling, and re-audit the `.kicad_dru` and netclass patterns against the actual
  hierarchical net names (`/sheet/NET`), including the EPD rails, before every fab.

- **DMM battery polarity check on every cell before first plug (assembly gate).** BATT1
  pin 1 = BAT+ (Adafruit convention), with prominent silkscreen. Reversed Chinese cells
  kill the TP4056 charger. This is a per-unit assembly invariant, not a one-time check:
  meter every cell's polarity against the BATT1 silkscreen before the first connection.

- **U102 (TP4056) pin-7 via-in-pad solder inspection (assembly risk).** Inspect the pin-7
  (CHRG_LED) solder joint on every assembled board. A via-in-pad here can wick solder and
  leave a starved or open joint that JLC's optical check may pass; confirm the joint under
  magnification (and reflow if starved) before trusting the CHRG indicator at bring-up.

- **GNSS RF feed (review B1): DONE.** ANT2, L201, and the U202 RF corner are now
  co-located in the GPS corner (short, direct feed away from the digital pads).
- **Enclosure GPS-antenna vs module collision (review B2): DONE.** GPS patch shrunk to
  12x12x4mm and the pocket pushed to model X82.8..97.3; the retaining wall now clears the
  e-paper module pocket by 1.3mm (SCAD assert `gps_pocket_clr >= 0`).
- **USB pair + ESD (review M2): DONE.** USB netclass added, pair classed, ESD relocated.
- **Fiducials + test points (review MINOR): DONE.** FID1-3 and TP1-5 placed.
- **WROOM extra decoupling (review MINOR): DONE.** Added distributed 100nF (C404/C405).
- **LoRa u.FL under-deck mating clearance (review M1): DONE (deck raised, not connector
  moved).** PCB analysis showed no legal case-wall edge spot for ANT1, so the panel deck
  was raised from a 4mm to a 6mm glass plane. Clearance from PCB top to the module
  underside is now GLASS_GAP - MODULE_TH = 4.95mm. A mated MHF1/u.FL receptacle+plug is
  ~2.5mm (up to ~4mm with a right-angle coax exit), so it clears with ~1 to 2.5mm margin,
  and a 2mm x 1.5mm deck relief channel above ANT1 lets the coax exit laterally to the
  LoRa flex on the top wall (so the vertical stack stays at connector height, not a cable
  loop). External thickness is now 18.9mm (<=19mm). Note: the deck was kept at 6.0mm, not
  raised to 6.3mm, because 6.3 would push the external thickness to 19.2mm past the 19mm
  cap and the 4.95mm underside clearance already clears a real mated connector; if the
  chosen antenna's specific connector-plus-strain stack measures >4.95mm at bring-up, add
  a local pocket-floor relief directly above ANT1 rather than raising the whole deck.

### Accepted DRC/silkscreen dispositions (re-confirm still valid at each fab)

These are dispositions, not open gates: they are accepted as-is but must be re-checked if
the layout or the fab's capability changes.

- **JLCPCB hole-to-copper clearance relaxed to 0.2mm board-wide (accepted disposition for
  106 track-to-via-hole violations).** On a 2-layer board JLCPCB's via drill-to-copper
  capability supports a 0.2mm hole-to-copper rule, so the design rule was relaxed to 0.2mm
  board-wide. This clears the 106 track-to-via-hole findings that the stricter default
  flagged; the relaxed value is within JLCPCB's stated 2-layer via capability. Re-confirm
  against the fab's current capability spec before ordering if the fab or stackup changes.

- **4 silkscreen edge-overhang findings waived (footprint-internal, cosmetic).** Four
  silkscreen-over-edge / edge-clearance findings are internal to their part footprints
  (refdes and outline silk on USBC1 / U401), are cosmetic only, and do not affect assembly
  or function. Waived. Re-check that no new silk clips a board edge if footprints move.
