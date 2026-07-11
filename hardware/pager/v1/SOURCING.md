# Bramble Pager v1 -- Hand-purchased parts sourcing

Parts that JLCPCB does NOT place and that must be bought separately and hand-installed:
the e-paper panel, both off-board antennas, and the LiPo cell. Sources below were
live-verified in July 2026; prices and stock drift, so re-check before ordering. The
JLC-assembled parts are in `kicad/production/bom.csv` and are not repeated here.

## E-paper panel: GoodDisplay GDEY0213B74 (SSD1680, 2.13", 250x122)

This is the exact panel the board was designed and footprinted against (0.3mm 24-pin tail,
180-degree fold, RESE = 2.2 ohm). Buy the raw panel:

- **microhello.com: ~$8.60.** Raw panel, exact part.
- **GoodDisplay AliExpress store: ~$9 to $13.** Exact fabbed panel, first-party.
- **WeAct 2.13" module: ~$10 (harvest fallback).** The WeAct EPaperModule ships the same
  raw GDEY0213B74 panel; harvesting the panel off the module is a valid fallback and its
  wiring is what confirmed the mirrored connector pin order. Costs more than the bare panel
  but is often faster to get.
- **Adafruit 4197: $22.50 (US-stock insurance).** Same panel, US warehouse, higher price;
  use only if you need it fast in the US.

Conditional-only alternate:

- **DEPG0213BN (Heltec Wireless Paper panel): do NOT order without confirming RESE.** It is
  pin-compatible on paper, but its RESE current-sense resistor value is unverified for this
  board's boost circuit. This design uses 2.2 ohm (R302); do not substitute DEPG0213BN
  unless you have confirmed it also wants 2.2 ohm. Treat as unverified until then.

Order panels early: they are the long-lead hand-purchased item.

## LoRa antenna (915MHz): Molex 105262-0001

- **DigiKey P/N 1052620001: $4.70, 25k+ in stock.** 79x10mm adhesive flex, 100mm MHF1
  pigtail, ground-plane independent. Mounts on the case wall; mates ANT1 (u.FL). This is
  the antenna the RF path was designed for.

## Battery: 603450-class 1200mAh 1S LiPo, JST-PH 2.0 connector

Target cell: ~603450 form factor, ~1200mAh, 1S, terminated in a JST-PH 2.0mm plug.

- **Liter 1200mAh (Amazon B09FLG39NX): ~$9.**
- **YDL 1200mAh (Amazon B07BTQFWGD): ~$9.**
- **Do NOT use Adafruit 258.** At 62mm long the cell is too long for the battery bay
  (the bay is sized for a ~603450 / 34x62x6mm envelope with the connector; the 258's body
  length overruns it). It is fine as an electrical reference only, not a fit part.

Polarity warning: on these vendor cells the JST-PH pin-1 polarity is effectively random.
BATT1 pin 1 = BAT+; a reversed cell kills the TP4056 charger. **DMM-check polarity on every
cell before the first plug** (this is also a per-unit assembly gate in `ERRATA.md`). Buy at
least two cells so a mis-polarized or dud cell does not stall bring-up.

## GPS antenna (1575.42MHz active): DECISION PENDING

This is an open decision, not a recommendation. Do not order a GPS antenna until the case
pocket size is settled.

- The current enclosure GPS pocket is **12x12x4mm**, which fits only PASSIVE patch antennas.
  The ATGM336H has an internal LNA, but a passive patch at that size gives marginal gain.
- The preferred ACTIVE antenna, **gnss.store ELT0176 (13.4x13.4x6mm, GPS+BDS, ~EUR 15.99)**,
  does not fit the 12x12x4mm pocket. Fitting it needs a case pocket grow plus a front bump,
  which is a mechanical change (and interacts with the B2 review fix that shrank the pocket
  to clear the e-paper module).

Resolve the tradeoff (grow the pocket for the active antenna vs accept a passive patch)
before buying. Note: the Molex 105262 is the 900MHz LoRa antenna and is the WRONG band for
GPS; do not reuse it here.
