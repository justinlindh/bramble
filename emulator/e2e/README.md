# emulator/e2e -- browser-level acceptance suite (Task 13)

Playwright suite that drives the Bramble emulator through a real headless
Chromium: the same UI a human opens, against the same live stack `make run`
serves (gosim broker + real IDF-linux firmware node processes + the built
React UI). Task 10's `make headless` suite (`emulator/ci/run_scenarios.sh`)
already asserts delivery at the framebuffer/log level; this suite proves the
thing a human actually *looks at* -- the rendered canvas -- and the thing a
human actually *does* -- clicking buttons, holding reset, loading a scenario
from the dropdown -- are correct, through the real DOM.

## Running it

```
cd emulator
make e2e
```

That builds the node binary, gosim, and the UI (same deps as `make run`),
installs Playwright's chromium if needed, picks a free port, boots gosim,
runs the suite, and tears the stack down. Exit 0 on pass.

To run directly (stack already built):

```
bash emulator/e2e/run_e2e.sh
```

## Layout

- `playwright.config.ts` -- one shared stack for the whole run (`workers: 1`,
  `fullyParallel: false`): a scenario load resets the broker's whole
  simulation state, so concurrent specs would race each other's scenarios.
- `globalSetup.ts` / `globalTeardown.ts` -- boot/kill gosim once for the run.
  gosim is spawned with `cwd` forced to the repo root (see `lib/stack.ts`'s
  doc comment) because it resolves each scenario's `firmware_nodes[].binary`
  path relative to its own process CWD, and killed via its process group so
  any live firmware node children die with it; `run_e2e.sh`'s own `trap` is a
  last-resort `pkill` safety net matching `emulator/scripts/smoke_live.sh`.
- `lib/` -- independent reference implementations, deliberately **not**
  sharing code with the app under test:
  - `font6x8.ts` -- transcribed straight from
    `components/display/include/font_6x8.h`.
  - `fbWire.ts` -- decodes the raw base64 `device_fb` wire payload from
    scratch (not `simulator/ui/src/device/framebuffer.ts`).
  - `glyphMatch.ts` -- rasterizes text with `font6x8.ts` and slides it over a
    decoded grid to find a pixel-exact occurrence (an independent
    reimplementation of the same technique `simulator/gosim/screen_assert.go`
    uses for the headless suite, written fresh for the browser layer).
  - `canvasRead.ts` -- reads back the actual `<canvas>` pixels via
    `getImageData` and classifies ink/paper/flash fills.
  - `wsCapture.ts` -- attaches to the page's live WebSocket and records every
    frame verbatim (both directions), independent of the app's own
    `useSimulation.ts` parsing. Also has the generic `waitFor()` poller used
    throughout instead of fixed sleeps.
  - `uiActions.ts` -- drives the real UI: load a scenario (dropdown + Load
    button + Play, see below), click a face button, hold-to-confirm reset.
  - `stack.ts` -- boot/teardown helpers used by `globalSetup`/`globalTeardown`.
- `specs/display-correctness.spec.ts` -- DISPLAY correctness (see below).
- `specs/functionality.spec.ts` -- FUNCTIONALITY (see below).
- `artifacts/` -- screenshots captured mid-test, committed as visual evidence.

## Node resolution note

The spec tree lives in `emulator/e2e/`, not inside `simulator/ui/` (where
`@playwright/test` is installed, reusing `webapp/`'s pinned version per the
task brief). Node resolves bare-specifier imports (`@playwright/test`) by
walking up from each importing *file's* own directory, not the invoking
shell's cwd -- `emulator/e2e/specs/../../../simulator/ui/node_modules` is
never on that walk. `run_e2e.sh` fixes this the standard way for a spec tree
living outside its dependency's package: `ln -sfn` a `node_modules` symlink
in `emulator/e2e/` pointing at `simulator/ui/node_modules`. `make clean`
removes it.

## What each spec asserts, and how

### display-correctness.spec.ts

**(a) canvas pixels equal the wire framebuffer for a rendered channel
message.** Loads `emu-channel-delivery` (provisioned sender + 2 receivers;
sender broadcasts "HELLO BRAMBLE"). Captures the raw `device_fb` WebSocket
frame off the wire (`wsCapture.ts`), decodes it from scratch (`fbWire.ts`),
and independently confirms "HELLO BRAMBLE" is really in those bits via a
from-scratch font-glyph search (`glyphMatch.ts` + `font6x8.ts`) -- this proves
delivery happened at the protocol level, with zero dependency on the DOM.
Then it reads back the actual `<canvas>` pixels the browser painted
(`canvasRead.ts`) and asserts **pixel-for-pixel equality** against the wire
decode across the full 250x122 frame. A decode, orientation, or bit-order bug
anywhere in the app's real pipeline (`framebuffer.ts`'s bit walk, a
transposed x/y in `Epaper.tsx`'s `putImageData`, an inverted polarity) would
make this fail almost everywhere, not just near the glyphs, because none of
the reference code (`fbWire.ts`, `glyphMatch.ts`, `font6x8.ts`) is shared
with the code under test.

**(b) a full refresh plays the inversion flash, black first, before content
resumes.** `epaperModel.ts` schedules a full refresh as black (t=0) -> white
(t=busy/2) -> content (t=busy), busy=3000ms
(`SSD1680_BUSY_MS_FULL`). This spec watches a node's first `kind:"full"`
`device_fb` event and samples the live canvas across the busy window,
asserting the observed sequence **starts black** and **eventually reaches
real rendered content** -- these are hard, deterministic assertions that a
reordered or dropped flash frame, or a broken wire-to-paint pipeline, would
break immediately (the black sample is taken at essentially t=0, so a
reordered schedule fails outright).

The white mid-phase is checked opportunistically (its ordering is asserted
*if* observed) rather than hard-required. This is a measured, not assumed,
scope decision -- 50+ seconds of captured wire events showed every
`kind:"full"` event superseded by the *next* `device_fb` event (main.c's
1Hz `SCREEN_MAIN` redraw, or a faster burst during boot) within roughly
500-1050ms, always before white's 1500ms mark. `Epaper.tsx`'s effect cancels
a frame's pending timers the instant a new one arrives (`Epaper.tsx`'s
`useEffect` cleanup on `[seq]`), so white is structurally pre-empted by real
firmware activity in this scenario, not flaky -- hard-requiring it would be
either an occasionally-failing test or, if loosened to be reliably true, a
weaker check than the black-first assertion already gives. See the honest
gap note in the source comment and in `task-13-report.md`.

### functionality.spec.ts

**mesh map still renders (no regression)** -- standalone, no scenario needed
(mesh view is the default, empty map). Regression sentinel per the task
brief.

**device cards, boot screen, buttons, reset persistence, and message
delivery** -- one `emu-channel-delivery` load carries all of:
- **(a)** exactly 3 device cards for the 3 firmware nodes (split-identity
  regression sentinel), cross-checked against the wire's distinct
  `node_joined` ids.
- **(b)** the "BRAMBLE" boot splash (`main.c`'s `display_draw_text_large`)
  found via glyph search in both the wire fb and the canvas.
- **(c)** clicking UP/DOWN/SELECT emits the exact
  `{ type:"btn", node, id, edge }` wire frame for both edges, **and**
  produces a visible firmware reaction: `main.c` logs `Button event: %d` on
  every dispatched press, forwarded to the browser as a console line.
- **(d)** RESET (hold past `PagerDevice.tsx`'s 800ms threshold) restarts the
  node (`button_virt.c`: a "reset" edge is `exit(0)`, not a UI event) and the
  **same** emu-link hello id re-attaches -- persistence via `flash.bin`,
  observed here through the live browser session rather than
  `smoke_live.sh`'s log-grepping. Runs *last*, after message delivery is
  already confirmed, so a mid-test reboot can't race that check.
- **(e)** the headline: real firmware A sends, real firmware B receives and
  renders "HELLO BRAMBLE," observed on >=2 distinct nodes' wire fb (mirroring
  `run_scenarios.sh`'s own `-min-nodes 2` bar) with a canvas spot-check on
  one of them. The strict pixel-exact proof lives in
  `display-correctness.spec.ts`; this step stays a presence check.

A screenshot of the device view is captured mid-test
(`artifacts/functionality-device-view.png`).

## A real bug this suite found and fixed

Face buttons looked interactive in the live UI but never reached firmware:
`simulator/gosim/extnode.go`'s `extConn.sendButton` had zero callers, and the
ws-command `Command` struct had no field for a `btn` message at all, so a
browser click's `{ type:"btn", node, id, edge }` frame fell through
`handleCommand`'s `default: unknown command` case and was silently dropped.
Headless testing never exercises buttons (there's no browser), so this was
invisible to `make headless`. Fixed in `simulator/gosim/sim.go` (`Command`
gained `Node`/`BtnID`/`Edge`, `cmdButton` routes to the right connection) and
`simulator/gosim/extnode.go` (`Broker.findByNode`, and `ec.node`'s write
moved under `b.mu` so the new cross-goroutine lookup is actually safe).
`go test ./...` and `emulator/ci/run_scenarios.sh` both stay green.

## Determinism

`make e2e` was run 3 consecutive times locally (plus once more via the raw
`run_e2e.sh` wrapper and once more via the full `make e2e` target,
5 total), all green, ~37s wall-clock each, well under the 4-minute budget.
No fixed sleeps gate delivery-dependent assertions; `lib/wsCapture.ts`'s
`waitFor()` polls with a generous bounded timeout instead.
