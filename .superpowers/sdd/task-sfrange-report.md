# Task: derive reception range from SF/BW link budget (gosim radio model)

Status: DONE

## Commit

See `git log -1` on `feat/phase2-flood-comparison` after this report is committed. Subject:

`fix(gosim): derive LoRa reception range from SF/BW link budget instead of fixed disk`

## The bug

`simulator/engine/sim_radio.c` gated deliverability on `dist > config->range`,
with `config->range` a hardcoded 150.0f (or whatever a scenario's JSON set).
`config->sf` / `config->bw_hz` fed only `bramble_calculate_airtime_us`
(time-on-air), never range. Changing SF/BW in a scenario changed how long a
frame occupied the channel but never whether a receiver could hear it at
all, which is physically wrong: higher SF buys more link budget (longer
range), wider bandwidth raises the noise floor (shorter range).

## The fix

`simulator/engine/sim_radio.c`:

- Added `radio_sensitivity_dbm(sf, bw_hz)`: SX127x/SX126x datasheet
  sensitivity at 125 kHz (SF7 -123, SF8 -126, SF9 -129, SF10 -132,
  SF11 -134.5, SF12 -137 dBm), bandwidth-adjusted by
  `+10*log10(bw_hz/125000)` dB, plus a single additive calibration constant
  `NOISE_MARGIN_DB`.
- Added `radio_derive_range(config)`: solves
  `range = 10^((tx_power - path_loss_d0 - sensitivity) / (10*path_loss_exp))`,
  the distance at which the existing `path_rssi_dbm` gradient crosses
  sensitivity.
- `radio_config_init` now computes `config->range = radio_derive_range(config)`
  after setting the default PHY (SF10/125 kHz), instead of hardcoding 150.0f.
- `radio_can_receive`, `channel_busy_at`, `radio_check_reception`, and the
  `sim_radio_broadcast` metrics path are all **unchanged**: they already
  gate on `config->range`, so once `range` is computed correctly nothing
  else needed to touch the comparisons. This was the "integrates most
  cleanly" option from the task brief.

`simulator/engine/sim_scenario.c` (`load_radio`): reordered so `sf`/`bw_hz`/
`tx_power_dbm`/`path_loss_exp`/etc. overrides are applied first, then:
if the scenario JSON has an explicit `"range"`, that wins (unchanged
override semantics, the escape hatch for topology tests); otherwise
`radio->range = radio_derive_range(radio)` using whatever PHY the scenario
just set. This is the "honor explicit override, derive otherwise" rule from
the brief.

`simulator/scenarios/generate.py`: no longer hardcodes `"range": 150` in the
emitted `radio` block. Added `--range` (omitted unless given, following the
generator's existing "omit unless explicit" rule for every other field with
a sim-side default). Default behavior: range is omitted and therefore
derived by the sim from `sf`/`bw_hz` (which are also omitted-unless-given).

`simulator/README.md`: updated the "Radio model" section to describe range
derivation instead of "disk-range gated" as if range were independent of
SF/BW.

## Sensitivity model and calibration constant

```
sensitivity(sf, bw) = base_sens(sf) + 10*log10(bw/125000) + NOISE_MARGIN_DB
```

`base_sens`: SF7 -123, SF8 -126, SF9 -129, SF10 -132, SF11 -134.5, SF12 -137
(all at 125 kHz, per the task brief / SX127x-SX126x datasheets).

`NOISE_MARGIN_DB = 38.9` dB, derived by solving for the additive offset that
makes the **default** link-budget params (tx_power 22 dBm, path_loss_d0
52 dB, path_loss_exp 2.9) land exactly on the simulator's existing
150-unit baseline at SF10/125 kHz:

```
margin = tx_power - path_loss_d0 - base_sens(SF10) - 10*path_loss_exp*log10(150)
       = 22 - 52 - (-132) - 29*log10(150)
       = 102 - 63.107
       = 38.89  (rounded to 38.9)
```

This constant exists because the simulator's own path-loss model
(`path_loss_d0_db=52`, `path_loss_exp=2.9`) is far lossier at short grid
distances than a real link budget with -132 dBm SF10 sensitivity implies
(the raw, uncalibrated budget would put SF10/125 kHz's range past 3000 grid
units, since the simulator's ~150-unit range was originally tuned for
test/gameplay scale, not literal RF physics). NOISE_MARGIN_DB folds that gap
into one number ("effective noise figure + implementation margin") so the
SF10 baseline is reproduced bit-for-bit while every other SF/BW combination
now moves relative to that baseline by the real datasheet deltas.

## Derived ranges

At the default link-budget params (tx_power 22 dBm, path_loss_d0 52 dB,
path_loss_exp 2.9), computed by `radio_derive_range`:

| SF | BW (kHz) | sensitivity (dBm) | range (units) |
|----|----------|--------------------|---------------|
| 7  | 125      | -84.1              | 72.8          |
| 7  | 250      | -81.1              | **57.8**      |
| 10 | 125      | -93.1              | **149.9** (baseline) |
| 10 | 250      | -90.1              | 117.1         |
| 10 | 500      | -87.1              | 92.2          |
| 12 | 125      | -98.1              | **223.1**     |

Ordering confirmed by unit test (`simulator/gosim/range_test.go`,
`TestDerivedRangeOrdering`): SF12 > SF10 > SF7 at fixed BW, and 125 kHz >
250 kHz at fixed SF, in both directions.

One deviation from the brief worth flagging: the brief's sanity target for
SF7/250 kHz was "roughly 60-75 units," reasoned from "a ~10.5 dB budget
difference (7.5 dB SF + 3 dB BW)." The brief's own datasheet table
(SF7 -123, SF10 -132) implies a 9 dB SF7-to-SF10 delta, not 7.5 dB; plus the
3.01 dB BW delta, the real budget difference is ~12 dB, which computes to
57.8 units, not 60-75. I calibrated `NOISE_MARGIN_DB` to hit the SF10
baseline exactly (the brief called that "critical, so existing results
stand") rather than split the difference to land SF7/250k inside 60-75; the
resulting 57.8 is close to that window and, more importantly, is well under
the 120-unit legacy grid spacing, which is what the demonstration below
actually depends on.

## Demonstration: the inversion this fix corrects

Reproduced using `simulator/scenarios/airtime-adaptive-50.json` (the
committed 50-node, 120-unit grid, 600 s, 2 msg/min legacy scenario) as a
base, varying only the `radio` block:

| Scenario | radio block | message_delivery_rate | receptions_ok | collisions |
|---|---|---|---|---|
| A0: committed baseline (unmodified file) | `sf`/`bw_hz` unset (SF10/125k default), `range: 150` | 0% (0/20) | 3529 | 2979 |
| A: same, `range` omitted (derives) | `sf:10, bw_hz:125000`, no `range` | **0% (0/20), metrics identical bit-for-bit to A0** | 3529 | 2979 |
| B: "old decoupled model" reconstruction | `sf:7, bw_hz:250000`, `range:150` (forced, reproducing the pre-fix bug) | **65% (13/20)** | 5707 | 1099 |
| C: fixed model | `sf:7, bw_hz:250000`, no `range` (derives to ~58) | **0% (0/20)**, `receptions_ok: 0` | 0 | 0 |

A vs. A0 confirms the calibration: switching from the hardcoded
`range: 150` to the derived value at SF10/125 kHz changes nothing (149.9 vs
150.0 is close enough that this specific grid's connectivity, and therefore
every downstream metric, is bit-for-bit identical).

B reconstructs exactly what the pre-fix code did: SF/BW changed only
airtime (SF7's much shorter ToA cuts channel occupancy roughly 12x versus
SF10), so with range still fixed at 150 the mesh stays "connected" and
route discoveries/data succeed far more often than the congested SF10
baseline: 65% delivery. This is the inversion: **the old model made SF7
look like a strictly better choice** (faster, same range) when in reality
SF7/250 kHz has less than half of SF10/125 kHz's actual range.

C is the same scenario with the fix applied (range omitted, so it derives
to ~57.8 units from SF7/250 kHz's link budget). At the legacy 120-unit grid
spacing, every node's nearest neighbor is now out of range (120 and 169
units diagonal, both > 57.8). Result: zero packets are ever received by
anyone (`receptions_ok: 0`, `collisions: 0` because nothing overlaps when
nothing is heard at all), 0% delivery. This is the physically correct
answer: the brief's own reasoning ("120 > ~65-unit SF7 range" -> mesh
disconnected) is confirmed, and the fix turns an artificially rosy 65%
result into the honest 0%.

(Note: the task brief speculated the old model would show "~45%" for this
case; I did not find or reproduce that exact figure, and don't have a
record of prior gosim output at these exact settings to explain the
discrepancy. What I reconstructed and measured directly is B's 65%, using
the fixed codebase's explicit-`range`-override escape hatch to exactly
replicate the pre-fix hardcoded-range-150 behavior at SF7/250 kHz. The
qualitative claim, an old decoupled model reporting artificially high
delivery for a SF/BW combination that is actually disconnected at that
spacing, is confirmed either way.)

Reproduce:
```bash
cd simulator/gosim && go build -o bramble-gosim .
# A0 (unmodified committed baseline):
./bramble-gosim --headless --scenario ../scenarios/airtime-adaptive-50.json
# B/C: copy airtime-adaptive-50.json, edit the "radio" block to
# {"sf":7,"bw_hz":250000,"range":150} (B) or {"sf":7,"bw_hz":250000} (C),
# then run the same way.
```

## Legacy scenarios: do they set range explicitly?

Checked every `simulator/scenarios/*.json` file: **every single one sets
`radio.range` explicitly** (mostly 150; also 120, 180, 200, 300, 500
depending on the scenario). None of them set `sf` or `bw_hz`. Consequence:
**zero existing scenario files are affected by this change at all** -
their explicit `range` always wins per the "explicit override wins" rule,
so their SF10/125 kHz (default) or whatever-range-they-set behavior is
byte-for-byte unchanged. The only place default derivation is now visible
is `radio_config_init`'s default (used by scenarios/harnesses that never
set an explicit range) and the regenerated behavior of
`simulator/scenarios/generate.py` output (which now omits `"range": 150` by
default; `--range` restores the old literal field for anyone who wants it).

## Tests

- New: `simulator/gosim/range_test.go`
  - `TestSF10BaselineRangeCalibration`: default (SF10/125k) `radio.range`
    lands within 2 units of 150; agrees with `radio_derive_range`.
  - `TestDerivedRangeOrdering`: SF12 > SF10 > SF7 at fixed BW; 125 kHz >
    250 kHz at fixed SF; SF10/125k ~150 (+/-2), SF12/125k ~223 (+/-5),
    SF7/250k ~58 (+/-5), and SF7/250k is meaningfully less than half of the
    SF10/125k baseline.
  - `TestSensitivityModelDatasheetValues`: at 125 kHz the SF7/SF10/SF12
    deltas exactly match the datasheet (9 dB, 5 dB); BW deltas match
    `10*log10(bw/125000)` (3.0103 dB at 250k, 6.0206 dB at 500k).
  - `TestLegacyGridDisconnectsAtSF7_250k`: at 120-unit spacing,
    `radio_can_receive` rejects an SF7/250k neighbor (range < spacing) and
    accepts an SF10/125k neighbor (range > spacing).
- Added harness accessors in `simulator/gosim/radio_harness.go`:
  `sensitivityDbm`, `deriveRange`, `rangeField`, and a package-level
  `radioCanReceive` wrapper (test files can't import "C" directly).
- Full suite: `cd simulator/gosim && go build -o bramble-gosim . && go test
  -count=1 ./...` -> `ok bramble-sim` (all pre-existing tests plus the new
  ones pass; no changes needed to any existing test).
- `clang-format -i` run via
  `bramble/runner-full:22.04-go126` on all three touched `.c`/`.h` files:
  no changes (formatting already matched project style).
- Did not run `test/run_all_tests.sh` (firmware host suite): no firmware C
  was touched, only `simulator/engine/` and `simulator/scenarios/`, per the
  task brief's guidance that this shouldn't be necessary.

## Concerns / follow-ups

- `NOISE_MARGIN_DB` is a single constant tuned to the *default* path-loss
  params (`path_loss_d0_db=52`, `path_loss_exp=2.9`, `tx_power_dbm=22`). A
  scenario that overrides those params *and* relies on derived range will
  get a range derived with the same margin but a different path-loss curve;
  that's the physically correct behavior (the margin is a receiver/noise
  property, not a function of the path-loss model), but it does mean the
  "SF10/125k ~150" anchor only holds exactly when those three defaults are
  also unchanged. No existing scenario does this (none override path-loss
  params today).
- The brief's illustrative "~45%" figure for the old-model SF7/250k
  disconnection demo doesn't match what I measured (65%, via a faithful
  reconstruction of the pre-fix behavior using the new explicit-range
  escape hatch). I don't have visibility into how that figure was
  originally produced, so I'm reporting my own measured number instead
  of trying to force a match. The qualitative point (old model
  overstates delivery for a range-incompatible SF/BW choice; new model
  correctly shows disconnection) holds regardless.
- The brief's SF7/250k sanity window (60-75 units) and the exact-baseline
  calibration (150 units at SF10/125k) are in mild tension given the
  datasheet table the brief itself specified (9 dB SF7->SF10 delta, not
  7.5 dB); see "Derived ranges" above. I prioritized the exact-baseline
  requirement since the brief called it "critical, so existing results
  stand," and 57.8 units is close to the stated window and, more
  importantly, still well under the 120-unit legacy grid spacing that the
  disconnection demo depends on.
