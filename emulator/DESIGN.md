# Bramble Emulator: Design (Phase 1, soft device)

Status: approved 2026-07-10. Implementation plan (archived):
`docs/archive/plans/emulator-plan.md`.
Phase 2 (QEMU true-VM backend) gets its own spec when phase 1 exits.

## 1. Goal

A virtual **Bramble Pager v1** that runs the real firmware source on a Linux host:
real `app_main()`, real `mesh_task.c`, real routing/crypto/UI logic, booted with a
virtual board profile. The pager is rendered in a browser as the physical device we
designed (case face, e-paper with true refresh behavior, clickable buttons), N
virtual pagers share a simulated RF ether with path-loss and collision modeling,
and the ether can bridge into the real physical mesh through a serial-attached
gateway node. Headless mode makes the whole thing a deterministic multi-node
integration rig for CI.

**The emulated device is the pager, specifically.** Look and feel means the pager:
GDEY0213B74 e-paper (250x122, SSD1680), UP/DOWN/SELECT face buttons, buzzer,
vibration motor, notification LED, GPS. The Heltec appears in this project exactly
once: as the sacrificial serial-attached RF gateway for the real-mesh bridge.
Nothing about the Heltec is mimicked.

Non-goals for phase 1:
- No Xtensa emulation (that is phase 2, QEMU device models, separate spec).
- No rewrite of the existing discrete-event simulator (`simulator/engine`); we
  extend gosim, we do not replace it.
- No WiFi/BLE emulation inside the virtual node (the ws_server RPC bridge can bind
  a real host socket later; out of scope now).
- No cycle-accurate timing claims. Timing fidelity is "airtime-priced wall clock",
  not instruction-level.

## 2. The emulated device

Everything below mirrors `main/boards/bramble_pager.h` and
`hardware/pager/v1/DESIGN.md`. The virtual board profile must stay in lockstep
with the real one (single source of truth: the real header, virtual profile
derives from it).

| Subsystem | Real hardware | Pins | Virtual backend |
|---|---|---|---|
| Radio | SX1262 (NiceRF, DIO2 RF switch) | CS 8, RST 12, BUSY 13, DIO1 14 | `radio_virt.c` implements `radio.h`, frames to the ether |
| Display | GDEY0213B74 e-paper, SSD1680, 250x122, 4-wire SPI | CS 4, D/C 5, RES# 6, BUSY 7 (active HIGH) | real SSD1680 engine (pure) + host transport, framebuffer to frontend |
| Buttons | SELECT/BOOT, UP, DOWN + RESET | 0, 21, 47 | `button_virt.c`, events from frontend |
| GPS | ATGM336H-5N31 (CASIC), UART 9600, P-FET power gate | TX 35, RX 36, EN 38 (LOW = on) | `gps_virt.c`, broker feeds NMEA from simulated position |
| Alerts | Buzzer 15, vibra 16, LED 48 | 15, 16, 48 | `indicator_virt.c`, state events to frontend |
| Battery | VBAT divider on GPIO1, ADC | 1 | `battery_virt.c`, scenario-scripted voltage |
| NVS/identity | esp32s3 NVS | : | IDF linux target NVS (file-backed) |

## 3. Architecture

```
 +--------------------+     emu-link (JSON lines over unix socket/TCP)
 |  virtual pager #1  |----------------------------+
 |  (real firmware,   |                            |
 |   IDF linux build) |                            v
 +--------------------+                   +-----------------+       +----------------+
 +--------------------+                   |  gosim broker   | <---> |  React UI      |
 |  virtual pager #N  |------------------>|  (the ether):   |  ws   |  device view,  |
 +--------------------+                   |  path loss,     |       |  mesh map,     |
 +--------------------+   serial          |  collisions,    |       |  consoles      |
 |  gateway Heltec    |------------------>|  airtime, RSSI  |       +----------------+
 |  (PHY passthrough) |                   +-----------------+
 +--------------------+                            ^
                                                   |
                                    sim_node harness nodes (existing gosim)
```

Three node types attach to one ether: full-firmware external nodes (this project),
existing sim_node harness nodes, and hardware gateways. Phase 2 QEMU nodes become a
fourth external-node flavor speaking the same protocol; broker and frontend do not
change.

## 4. Node runtime

Primary path: **ESP-IDF `linux` preview target** (`idf.py --preview set-target
linux`). It provides FreeRTOS-as-pthreads, NVS, esp_timer, esp_event on host,
which is what `mesh_task.c`'s task/queue structure needs. All 14 hardware-touching
files in the tree are already `#ifdef ESP_PLATFORM`-gated and `crypto_host.c`
(OpenSSL) exists, so the port surface is the virtual drivers plus whatever
`main/` assumes about the device environment.

Known IDF-linux caveats we accept and design around:
- Cooperative preemption: busy-loops never yield. Any spin-wait found during the
  spike gets a `vTaskDelay` and that is a legitimate firmware fix, not a hack.
- `driver` component is mock-only on host: no SPI/GPIO/UART. Correct by
  construction here, virtual drivers sit at the `radio.h`/`display.h`/component
  seams, never at the bus level.
- Tick-granularity timing: fine for protocol logic, airtime is priced by the
  broker anyway.

Fallback (decided at the Task 1 gate, not before): extend the existing
`test/stubs/` gcc harness with a FreeRTOS shim, portduino-style. Costlier, fully
under our control. The spike exists to make this decision with evidence.

## 5. Virtual peripherals and seams

One new host-only component, `components/emu_link/`, owns the broker connection:
JSON-lines over a unix socket (or TCP, `EMU_BROKER` env), a reader thread
dispatching to registered handlers, thread-safe writes. Every virtual driver is a
thin client of emu_link and implements an existing firmware interface unchanged:

- `radio_virt.c` implements `components/radio/include/radio.h`. TX serializes the
  frame plus modulation params to the broker; tx_done fires after the broker acks
  with the priced time-on-air (which must agree with `radio_airtime.c`). RX frames
  from the broker invoke the registered rx callback with broker-computed
  RSSI/SNR. CAD is answered by the broker (channel busy model). All TX still
  funnels through the real `tx_gate` logic.
- Display: see section 6. The e-paper pipeline is the one place we restructure
  firmware, because the pager's SSD1680 driver does not exist yet and we want the
  real driver logic exercised by the emulator, not a parallel fake.
- `button_virt.c` implements the `button` component's contract, fed by broker
  button events (`up`, `down`, `select`, `reset`; reset restarts the node
  process).
- `gps_virt.c` implements the `gps` component's device half; the pure
  `nmea_parser.c` is used as-is. The broker synthesizes NMEA (RMC/GGA) from the
  node's scenario position, respecting the GPIO38 power-gate state (gated off = no
  sentences, exactly like the P-FET).
- `battery_virt.c` and `indicator_virt.c`: scripted VBAT millivolts in; LED
  on/off, buzzer on/off+frequency, vibra on/off events out.

## 6. E-paper pipeline (firmware work with real-hardware payoff)

`components/display/` gains the pager's real display driver, split for
testability:

- `ssd1680_engine.c` (pure, host-testable): framebuffer (250x122, 1bpp), dirty
  tracking, refresh policy (partial refresh with a full refresh forced every N
  partials and on init, per GDEY0213B74 datasheet guidance), SSD1680 command
  stream generation (the ~20-command vocabulary, RAM windowing, LUT selection).
  Output: a command/data byte stream plus a "refresh kind" annotation.
- `ssd1680_io.c` (ESP_PLATFORM): 4-wire SPI transport, D/C and RES# lines, BUSY
  wait (active HIGH). This is the driver the real pager boards will use when they
  arrive; the emulator project delivers it early.
- `display_virt.c` (host): consumes the engine's output. It forwards the resolved
  framebuffer plus refresh kind (partial/full) and engine-computed busy duration
  to the broker. The frontend, not the firmware, renders e-paper physics.

Both `ssd1680_io.c` and `display_virt.c` present the existing `display.h`
interface upward; UI code cannot tell them apart.

## 7. The ether (gosim evolution)

`simulator/gosim` gains:

- **External node type**: a listener accepting emu-link connections; each becomes
  a node in the existing radio model with a position, participating in path-loss,
  collision, and airtime accounting alongside sim_node harness nodes.
- **Real-time mode**: scenarios containing external nodes run on the wall clock
  (virtual-time acceleration remains available for pure harness scenarios). This
  is the honest cost of full-firmware processes and is stated, not hidden.
- **Process supervisor**: scenario files can declare `"firmware"` nodes; gosim
  spawns the built node binary per instance (unique node dir for NVS/identity,
  env pointing at the broker socket), captures stdout as the per-node console,
  restarts on the reset button.
- **Gateway client**: a serial attachment (`--gateway /dev/ttyUSB0`) that speaks
  the PHY passthrough protocol to a real node, making the physical RF channel one
  more member of the ether. Frames from the real mesh enter the collision model
  as received; broker frames destined for air go out the gateway's real radio.

## 8. emu-link protocol (v1)

JSON lines, one object per line, `t` discriminates. Node to broker: `hello`
(node id, fw version, caps), `tx` (b64 payload, freq, sf, bw, cr, power), `cad`,
`fb` (seq, kind partial|full, b64 packed 1bpp 250x122, busy_ms), `ind` (led,
buzzer_hz, vibra), `gpsgate` (on|off), `log` (line). Broker to node: `rx` (b64
payload, rssi, snr, freq), `txdone` (toa_ms), `cadres` (busy), `btn` (id, edge),
`nmea` (sentence), `batt` (mv), `time` (epoch ms at attach). Unknown message
types are ignored by both sides (forward compatibility with phase 2). The
protocol version rides in `hello`; the broker refuses mismatches loudly.

## 9. Frontend (device view)

The simulator's React UI (same stack as the webapp: React + zustand) gains a
device view rendering each firmware node as the physical pager:

- Face SVG derived from the actual case geometry (`hardware/pager/v1/case/
  pager_case.scad` dimensions: window placement over the e-paper active area,
  three face buttons, LED aperture). Buttons are clickable/keyboard-drivable and
  send `btn` events; RESET is in a corner, hold-to-confirm.
- E-paper canvas with modeled physics: partial refresh applies after the engine's
  busy duration; full refresh plays the inversion flash (black/white/black) over
  its 2-4s duration; ghosting accumulates as a low-alpha residue of prior frames
  until a full refresh clears it. Parameters live in one tunable table seeded
  from the GDEY0213B74 datasheet, to be corrected against real panels later.
- LED renders as a lit aperture, vibra as a subtle device shake, buzzer as a Web
  Audio tone at the reported frequency (muted by default).
- Per-node console (firmware log stream) and the existing mesh map showing all
  node types, link quality, and in-flight frames.

## 10. Hardware bridge (PHY passthrough)

Firmware gains a passthrough mode: RX frames are forwarded up the serial/RPC link
with their radio metadata; frames received over serial are transmitted raw.
Because this transmits arbitrary frames on the real channel, it is gated hard:
disabled by default, enabled only via authenticated RPC, auto-expires after a
configurable window (default 30 min), never persists across reboot, and refuses
while the node holds a live channel identity unless explicitly forced. The
sacrificial bench Heltec is the intended gateway.

## 11. Testing and CI

- Unit: emu_link framing, ssd1680_engine command streams asserted against
  datasheet sequences, radio_virt airtime agreement with `radio_airtime.c`,
  broker RSSI/collision pricing for external nodes.
- Scenario (headless): gosim spawns N firmware nodes, drives scripted button/rx
  events, asserts on delivered messages AND on framebuffer content (the fb stream
  is part of the scenario API; "the message renders on the pager screen" is an
  assertable fact). First scenarios: 3-pager channel delivery with screen
  assertion; DM session desync repro (the class of bug this rig exists to catch).
- CI: scenario suite runs headless in the existing CI alongside the 103 host
  tests. Real-time mode means wall-clock cost; the suite budget is minutes, kept
  by capping scenario durations.

## 12. Phase 2 contract (QEMU)

What phase 1 must not break: a QEMU node is an external node. The SX1262 QEMU
device model speaks emu-link out a chardev socket; the SSD1680 model emits `fb`
messages; the GPIO model consumes `btn`. Broker and frontend are already done for
it. Phase 2's scope is QEMU device-model work only (GPSPI2 controller, SX1262 and
SSD1680 SSI slaves, GPIO), and it runs the exact flashable pager .bin. Separate
spec, separate plan, after phase 1 exit.

## 13. Risks

1. **IDF linux target vs mesh_task.c** (top risk): experimental target, 6.6k-line
   task file never run on host. Mitigated by making this the first task, a spike
   with an explicit go/no-go gate and a concrete fallback (test/stubs FreeRTOS
   shim).
2. **Real-time scenario flakiness in CI**: wall-clock scheduling jitter. Mitigate
   with generous assertion windows and airtime pricing from the broker (which is
   deterministic), never from node-side timing.
3. **E-paper model fidelity**: refresh/ghosting parameters are datasheet-derived
   until panels arrive. Displayed as a model, corrected later; not a blocker.
4. **Passthrough misuse**: mitigated by the gating in section 10.
5. **gosim real-time mode** touches the simulator's event loop: guarded by
   keeping virtual-time paths untouched for existing scenarios (existing scenario
   suite must stay green).

## 14. Phase 1 exit criteria

Three virtual pagers plus the bridged gateway on one ether; a channel message sent
from the real fleet arrives on a virtual pager and renders on its e-paper with
the refresh flash; a reply composed via clicked buttons reaches the real fleet;
headless scenario suite (including the screen-assertion and DM desync scenarios)
green in CI; `ssd1680_engine`/`ssd1680_io` merged and ready for first hardware.

## 15. Phase 1 status (as-built, 2026-07-11)

Phase 1 is COMPLETE and merged to `feature/emulator-v1`. All twelve plan tasks
plus the browser E2E (task 13) shipped, each through TDD + independent review.
Gates at completion: 110 host test suites green, gosim `go test` green, UI
vitest 20/20, the linux node and esp32s3 pager builds green, the headless
scenario suite and the browser E2E both green (E2E deterministic across 6 runs).

What the emulator proves today: three unmodified-firmware pagers boot on the
linux target, attach to the gosim ether with real cryptographic identities that
survive restart, provision a shared network key, and exchange real channel
messages that render on each other's e-paper, observed pixel-exact through a
real browser. Buttons reach the firmware; RESET restarts with identity intact.

Deviations from the plan as written, and why:
- Provisioning + scripted send use a boot-time `EMU_NETWORK_KEY` env seed and
  scenario-scripted autosend, not the emu-link control-message path section 8
  originally implied. The env path was sufficient for the headless/CI scenarios
  and reuses the real `network_key_set_provisioned`. The richer emu-link
  control-message path (runtime provision/send, needed for fully interactive
  UI-driven sends) is designed and gate-green but held out of this branch; it is
  preserved on tag `emulator-task13-control-path` for a follow-on.
- E-paper white-flash fidelity gap: on a real panel a full refresh drives the
  BUSY line for ~3s and the controller cannot redraw during it, so the
  black/white/content flash plays fully. `display_virt.c` does not model BUSY
  blocking, and the firmware redraws every ~500-1050ms, faster than
  `EPD_MODEL`'s 1500ms white onset, so the white mid-phase is structurally
  pre-empted and never paints in the emulator. The E2E asserts black-first and
  eventual-content (both hard) and documents the white phase as an observed,
  measured gap. Close it by modeling a BUSY-equivalent block in `display_virt.c`
  when calibrating against real panels.
- Bug found and fixed by the E2E (task 13): face-button ws messages were
  silently dropped server-side (no `Command` btn fields, `sendButton` had no
  caller). This was invisible to headless testing and is exactly the class of
  defect the browser-level suite exists to catch.

Phase 2 (QEMU true-VM backend running the exact flashable image) remains a
documented follow-on per section 12; it is not started.
