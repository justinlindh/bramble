# Bramble Emulator Phase 1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A virtual Bramble Pager v1 (real firmware on Linux, e-paper-faithful browser device view, N-node virtual ether via gosim, real-mesh serial bridge, headless CI scenarios), per emulator/DESIGN.md.

**Architecture:** Real firmware compiled with the ESP-IDF linux target; virtual drivers at the existing `radio.h`/`display.h`/component seams speaking a JSON-lines "emu-link" protocol to an evolved gosim broker; React device view; PHY-passthrough gateway firmware mode.

**Tech Stack:** C (ESP-IDF 5.4.0, linux preview target), Go (gosim), React + zustand (simulator UI), Unity + ASan (host tests, existing `test/` harness).

## Global Constraints

- No em dashes anywhere in committed file content (repo hook rejects them). Use colons or commas.
- ESP-IDF is 5.4.0; host/device split is `#ifdef ESP_PLATFORM` (repo convention), never new custom macros. Amendment (Task 1 finding, 2026-07-11): the IDF linux target defines ESP_PLATFORM too, so driver-touching device halves gate with `#if defined(ESP_PLATFORM) && !defined(CONFIG_IDF_TARGET_LINUX)` (IDF's own macro); pure IDF-API device code (NVS, crypto, FreeRTOS) compiles unchanged on linux and runs real device paths. The plain-gcc test harness defines neither symbol and is unaffected.
- The 103 existing host tests (`test/run_all_tests.sh`) and the existing gosim scenario suite must stay green after every task.
- Firmware interfaces `components/radio/include/radio.h` and `components/display/include/display.h` must not change signatures; virtual backends implement them as-is.
- The emulated device is the pager (main/boards/bramble_pager.h). Pins of record: radio CS 8/RST 12/BUSY 13/DIO1 14, EPD CS 4/DC 5/RES 6/BUSY 7 (active HIGH), buttons SELECT 0/UP 21/DOWN 47, buzzer 15, vibra 16, LED 48, GPS TX 35/RX 36/EN 38 (LOW = on), VBAT GPIO1.
- emu-link protocol per DESIGN.md section 8; unknown message types ignored; version in `hello`.
- Commit per task minimum; branch `emulator-v1`; merge policy: CI green, /code-review for complicated diffs.

---

### Task 1: Spike, firmware boots on the IDF linux target (GO/NO-GO GATE)

**Files:**
- Create: `emulator/node/CMakeLists.txt`, `emulator/node/sdkconfig.defaults`, `emulator/node/README.md`
- Modify: `main/CMakeLists.txt` (host-conditional sources), whatever `main/main.c`/`main/mesh_task.c` guards the spike proves necessary (each such edit must be an `#ifdef ESP_PLATFORM` gate or a genuine firmware fix like a missing `vTaskDelay`, never a behavior fork)
- Test: boot log assertion script `emulator/node/spike_check.sh`

**Interfaces:**
- Consumes: existing firmware tree, `crypto_host.c`, ESP_PLATFORM gating.
- Produces: a `bramble-node` linux binary whose `app_main` starts `mesh_task` with radio calls hitting a temporary no-op `radio.h` stub (in-tree, `emulator/node/radio_null.c`); boots to "mesh task started" log line and idles without crashing or spinning a core.

- [ ] **Step 1:** Create the linux-target project wrapper: `emulator/node/` is an IDF project whose main component pulls `main/` + `components/` sources, `idf.py --preview set-target linux`.
- [ ] **Step 2:** Write `radio_null.c` (every `radio.h` function returns success, rx callback never fires) and stub `display.h` the same way (`display_null.c`). These are spike scaffolding, deleted in Tasks 4/5.
- [ ] **Step 3:** Build. Triage every failure into: (a) missing ESP_PLATFORM gate, (b) IDF-linux gap, (c) real firmware bug. Fix (a)/(c) in-tree, log (b) in `emulator/node/README.md`.
- [ ] **Step 4:** Run under `timeout 60`. Expected: identity generated (host NVS), mesh task started, discovery beacon attempted through the null radio, CPU near-idle (`top` check in `spike_check.sh`). ASan-clean.
- [ ] **Step 5:** `test/run_all_tests.sh` still green. Commit.
- [ ] **Step 6 (GATE):** Write the verdict in `emulator/node/README.md`: linux target viable (list of caveats hit) OR fallback to test/stubs FreeRTOS shim required (then STOP, revise plan Tasks 2+ around the shim before proceeding).

### Task 2: emu_link component

**Files:**
- Create: `components/emu_link/emu_link.c`, `components/emu_link/include/emu_link.h`, `components/emu_link/CMakeLists.txt`
- Test: `test/test_emu_link.c` (register in `test/CMakeLists.txt`)

**Interfaces:**
- Produces (exact, consumed by Tasks 4-6):
```c
int  emu_link_connect(const char *node_id, const char *caps_csv); /* EMU_BROKER env: unix:/path or tcp:host:port */
typedef void (*emu_link_handler_t)(const cJSON *msg, void *ctx);
int  emu_link_on(const char *type, emu_link_handler_t h, void *ctx); /* one handler per type */
int  emu_link_send(cJSON *msg);   /* adds "t" callers set; thread-safe; takes ownership */
void emu_link_close(void);
```
- Host-only component (`#ifndef ESP_PLATFORM` body), reader thread dispatches by `t`.

- [ ] **Step 1:** Write failing tests: framing (one JSON object per line, split reads), handler dispatch, unknown-type ignored, thread-safe concurrent sends (ASan/TSan-friendly), `hello` emitted on connect with protocol version 1.
- [ ] **Step 2:** Run tests, verify they fail. Implement against a socketpair test double. Tests green.
- [ ] **Step 3:** Commit.

### Task 3: virtual pager board profile

**Files:**
- Create: `main/boards/virtual_pager.h`
- Modify: `main/board.c`, `main/Kconfig.projbuild` (`BRAMBLE_BOARD_VIRTUAL_PAGER`), `emulator/node/sdkconfig.defaults`
- Test: extend `test/test_board_profiles.c`

**Interfaces:**
- Produces: `virtual_pager.h` returning the bramble_pager capability set (`BOARD_CAP_GPS | BOARD_CAP_EPD | ...` exactly matching `bramble_pager.h`, including buzzer/vibra/LED and button pins) so firmware code paths match the real device; pin numbers copied verbatim (they are labels on host, but keeping them identical keeps logs/diagnostics comparable).

- [ ] **Step 1:** Failing test: profile parity assertion (every capability flag and pin in `bramble_pager.h` equals `virtual_pager.h` except a new `BOARD_CAP_VIRTUAL` marker flag).
- [ ] **Step 2:** Implement header + Kconfig + board.c wiring. Tests green. Node binary boots with the profile. Commit.

### Task 4: radio_virt

**Files:**
- Create: `components/radio/radio_virt.c` (host-only)
- Delete: `emulator/node/radio_null.c`
- Modify: `components/radio/CMakeLists.txt`
- Test: `test/test_radio_virt.c`

**Interfaces:**
- Consumes: `emu_link.h`, `radio.h`, `radio_internal.h` (TX funnels through real `tx_gate`), `radio_airtime.c`.
- Produces: full `radio.h` implementation: `tx` message out with modulation params; `txdone` in fires the tx-done callback; `rx` in fires the rx callback with broker RSSI/SNR; `cad`/`cadres` for CAD; `radio_sleep`/state machine honored.

- [ ] **Step 1:** Failing tests against a fake broker (socketpair): tx emits correct JSON (payload b64, sf/bw/cr/freq/power from `radio_config_t`); local airtime estimate (via `radio_airtime`) within 1ms of the value a reference vector says the broker will price; rx dispatch; CAD; state transitions.
- [ ] **Step 2:** Implement, tests green, delete `radio_null.c`, node binary now connects to a netcat fake broker and beacons. Commit.

### Task 5: SSD1680 pipeline (engine + device transport + virtual backend)

**Files:**
- Create: `components/display/ssd1680_engine.c`, `components/display/include/ssd1680_engine.h`, `components/display/ssd1680_io.c` (ESP_PLATFORM), `components/display/display_virt.c` (host)
- Delete: `emulator/node/display_null.c`
- Modify: `components/display/CMakeLists.txt` (select by CONFIG_BRAMBLE_BOARD_*), `main/boards/bramble_pager.h` + `virtual_pager.h` (EPD pin struct if not yet present)
- Test: `test/test_ssd1680_engine.c`

**Interfaces:**
- Produces (exact):
```c
typedef enum { SSD1680_REFRESH_NONE, SSD1680_REFRESH_PARTIAL, SSD1680_REFRESH_FULL } ssd1680_refresh_t;
typedef struct { uint8_t cmd; const uint8_t *data; size_t len; } ssd1680_op_t;
void ssd1680_engine_init(void);                        /* forces first refresh FULL */
void ssd1680_engine_pixel(int x, int y, bool on);      /* 250x122, 1bpp */
ssd1680_refresh_t ssd1680_engine_flush(const ssd1680_op_t **ops, size_t *n_ops, uint32_t *busy_ms);
const uint8_t *ssd1680_engine_fb(void);                /* 3904 bytes, row-major, 32B/row */
```
- `ssd1680_io.c` and `display_virt.c` both implement `display.h` on top of the engine (text via the existing `font_6x8.h` path, as ssd1306 does). Refresh policy: FULL on init and every 10th flush, PARTIAL otherwise; busy_ms 500 partial / 3000 full (datasheet-seeded constants in one table).

- [ ] **Step 1:** Failing engine tests: init command sequence matches GDEY0213B74 datasheet (driver output control for 250 sources, RAM window, border, BS1 4-wire assumptions); partial vs full LUT/Display Update Control 2 values; policy (10th flush is FULL); fb byte layout.
- [ ] **Step 2:** Implement engine (pure C, no IDF includes). Tests green.
- [ ] **Step 3:** Implement `ssd1680_io.c` (SPI writes, D/C, RES# pulse, BUSY active-HIGH wait with timeout). Compiles for esp32s3 target (`idf.py build` for build-pager); cannot run without hardware, flagged for bring-up.
- [ ] **Step 4:** Implement `display_virt.c`: flush sends `fb` message (seq, kind, b64 fb, busy_ms) via emu_link. Node binary boot screen visible as an `fb` message on the fake broker. Commit.

### Task 6: remaining virtual peripherals

**Files:**
- Create: `components/button/button_virt.c`, `components/gps/gps_virt.c`, `components/battery/battery_virt.c`, `components/indicators/indicator_virt.c` (new tiny component wrapping LED/buzzer/vibra GPIO writes; device impl is a trivial gpio wrapper added alongside)
- Modify: respective CMakeLists; alert call sites in `main/` route through `indicators` (mechanical refactor, device behavior unchanged)
- Test: `test/test_gps_virt.c`, `test/test_indicators.c`

**Interfaces:**
- Consumes: emu_link (`btn`, `nmea`, `batt` in; `ind`, `gpsgate` out).
- Produces: button events into the existing button component contract; NMEA lines into the real `nmea_parser.c`; battery mv into the battery API; `indicator_set_led(bool)`, `indicator_buzzer(uint32_t hz_or_0)`, `indicator_vibra(bool)`.

- [ ] **Step 1:** Failing tests: gps power gate (gpsgate off = nmea handler not registered/dropped, mirroring the P-FET), indicator event emission, button edge dispatch, reset button triggers clean `exit(0)` (supervisor restarts).
- [ ] **Step 2:** Implement all four + the indicators device wrapper. Full-firmware node now boots with zero null stubs. Both device targets still build (`build-pager`, heltec). Commit.

### Task 7: gosim external nodes, real-time mode, supervisor

**Files:**
- Create: `simulator/gosim/extnode.go`, `simulator/gosim/supervisor.go`, `simulator/gosim/gateway.go` (stub, filled in Task 9)
- Modify: `simulator/gosim/` event loop (real-time clock source), scenario schema + loader, `simulator/scenarios/` (new example `emulator-3-pagers.json`)
- Test: `simulator/gosim/extnode_test.go`, `supervisor_test.go`

**Interfaces:**
- Consumes: emu-link protocol (broker side), existing `sim_radio` path-loss/collision model.
- Produces: scenario node type `{"type":"firmware","binary":"emulator/node/build/bramble-node","count":3,"positions":[...]}`; broker listens on a unix socket, prices TOA deterministically, sends `txdone`/`rx`/`cadres`; supervisor spawns/restarts processes with per-node `NODE_DIR` (NVS) and `EMU_BROKER`; wall-clock mode auto-enabled when any external node exists, virtual-time path untouched otherwise.

- [ ] **Step 1:** Failing Go tests: two fake extnode conns, A tx priced and delivered to B with model RSSI; collision when overlapping airtime; existing virtual-time scenario tests untouched and green.
- [ ] **Step 2:** Implement extnode + real-time clock source. Tests green.
- [ ] **Step 3:** Implement supervisor (spawn, console capture, restart-on-exit for reset). Integration check: `./bramble-gosim --headless --scenario scenarios/emulator-3-pagers.json` shows three nodes attach, beacon, discover each other (assert on broker log). Commit.

### Task 8: frontend device view

**Files:**
- Create: `simulator/ui/src/device/PagerDevice.tsx`, `Epaper.tsx`, `epaperModel.ts`, `pagerFace.ts` (face geometry constants derived from `hardware/pager/v1/case/pager_case.scad`: window offset, button centers, LED aperture)
- Modify: `simulator/ui` routing/store (device view per firmware node; ws plumbing for `fb`/`ind`/console streams and `btn` sends)
- Test: `simulator/ui` vitest: `epaperModel.test.ts`, `PagerDevice.test.tsx`

**Interfaces:**
- Consumes: broker websocket events `{node, fb|ind|log}`; sends `{node, btn}`.
- Produces: `epaperModel.ts` exact contract: `applyFrame(state, fb, kind, busy_ms) -> {frames: CanvasFrame[], ghost: number}` (partial: single frame after busy_ms with ghost accumulation +0.06/frame capped 0.3; full: inversion flash sequence black/white/content over busy_ms, ghost reset to 0). Tunables in one exported table `EPD_MODEL`.

- [ ] **Step 1:** Failing vitest for `epaperModel` (frame sequences, ghost math) and component test (button click emits `btn`, canvas updates on fb).
- [ ] **Step 2:** Implement model + components (device face SVG with clickable SELECT/UP/DOWN, hold-to-confirm RESET, LED glow, vibra shake CSS, muted-by-default buzzer via Web Audio). Tests green.
- [ ] **Step 3:** Manual gate: run 3-pager scenario, open UI, screenshot the device view rendering a boot screen and a received message. Attach screenshot to the PR. Commit.

### Task 9: hardware bridge (PHY passthrough + gateway client)

**Files:**
- Create: `components/radio/phy_passthrough.c` (+ RPC methods `phy.enable`/`phy.disable`/`phy.status` in `main/rpc_methods.c`)
- Modify: `simulator/gosim/gateway.go` (serial client: enables passthrough via authenticated RPC, translates frames <-> ether)
- Test: `test/test_phy_passthrough.c`, `simulator/gosim/gateway_test.go` (against a scripted fake serial)

**Interfaces:**
- Consumes: `radio_internal.h` raw TX, RX tap before mesh processing.
- Produces: gated passthrough per DESIGN.md section 10: default off, authenticated RPC enable with TTL (default 30 min), no persistence, refuses with live channel identity unless `force:true`; frame RPC events carry payload + rssi/snr/freq.

- [ ] **Step 1:** Failing firmware tests: gating (disabled by default, TTL expiry, identity refusal, force override), frame round-trip encoding.
- [ ] **Step 2:** Implement firmware side; both device builds green; host tests green. Commit.
- [ ] **Step 3:** Implement gateway.go against fake serial; test: real-mesh frame enters ether and is delivered to a virtual node per the model. Commit.
- [ ] **Step 4 (hardware, user-present):** bench Heltec flashed (remember: V3 is flash-encrypted, use --encrypt; kill stale port monitors first), `--gateway /dev/ttyUSB*`, live frame from the real fleet reaches a virtual pager screen. Findings logged in `emulator/README.md`.

### Task 10: headless scenarios + CI

**Files:**
- Create: `simulator/scenarios/emu-channel-delivery.json`, `simulator/scenarios/emu-dm-desync.json`, `emulator/ci/run_scenarios.sh`
- Modify: CI workflow (new job: build linux node + gosim, run scenario suite), scenario schema (assertion blocks: `expect_delivered`, `expect_screen_contains` via fb OCR-free bitmap match of rendered text using the same font_6x8 glyphs)
- Test: the scenarios themselves + one intentionally-failing assertion test proving the harness fails loudly

**Interfaces:**
- Consumes: everything above.
- Produces: `run_scenarios.sh` exit code gates CI; scenario assertion vocabulary documented in `simulator/README.md`.

- [ ] **Step 1:** Implement `expect_screen_contains` (render expected string with font_6x8, search fb bitmap). Failing-then-green harness test.
- [ ] **Step 2:** Channel-delivery scenario: 3 pagers, node A sends, assert B and C screens show the message within 30s wall clock. Green locally 10/10 runs.
- [ ] **Step 3:** DM desync repro scenario: stale one-sided session per the known bug's repro notes; assert the heal path (post-#138) delivers. Green.
- [ ] **Step 4:** Wire CI job, budget under 5 minutes. Commit.

### Task 11: docs + phase 1 exit review

**Files:**
- Create: `emulator/README.md` (quick start: build node, run scenario, open UI, bridge a gateway)
- Modify: root `README.md` pointer, `emulator/DESIGN.md` (record deviations), memory files
- Test: quick-start walkthrough executed verbatim in a clean checkout

**Interfaces:** none new.

- [ ] **Step 1:** Write README, execute it verbatim, fix drift.
- [ ] **Step 2:** Check every exit criterion in DESIGN.md section 14; record evidence (scenario runs, screenshots, gateway session log).
- [ ] **Step 3:** Final commit; PR(s) per merge policy; propose phase 2 (QEMU) spec kickoff.

### Task 12: run targets (Makefile) + Docker packaging

Added 2026-07-11 per user: simple run targets that verify prerequisites and launch.

**Files:**
- Create: `emulator/Makefile`, `emulator/Dockerfile`, `emulator/docker-compose.yml`
- Modify: `emulator/README.md` (quick start becomes `make` targets)
- Test: each target run on the workstation; compose brought up from scratch

**Interfaces:**
- Consumes: node binary build (Task 1), gosim (Task 7), UI build (Task 8), scenarios (Task 10).
- Produces (exact target names):
  - `make check`: verify prerequisites (idf.py + linux target, go, node, jq), print what is missing and how to get it, exit nonzero if unusable
  - `make node`: build the linux firmware node binary
  - `make broker`: build gosim
  - `make ui`: build the React UI
  - `make run`: check + build all + launch the 3-pager scenario with UI, print the URL
  - `make headless`: run the CI scenario suite locally
  - `make clean`
- Docker: one image containing ESP-IDF (linux target only), Go, Node; compose service mirroring `simulator/docker-compose.yml` conventions so `docker compose up --build` inside `emulator/` is the zero-prerequisite path.

- [ ] **Step 1:** Makefile with prereq-checking `check` target; every other target depends on it. Each target is a thin wrapper over the canonical commands documented in README (no logic that exists nowhere else).
- [ ] **Step 2:** Dockerfile + compose; build from clean context; document image size expectation in README.
- [ ] **Step 3:** Execute `make run` and `docker compose up --build` from scratch; fix drift; commit.
