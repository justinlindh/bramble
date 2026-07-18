// display-correctness.spec.ts
//
// PLAN.md Task 13, deliverable 2: proves the DISPLAY is correct, at the
// browser/canvas level (what a human looking at the app actually sees), not
// just the headless framebuffer level Task 10's scenario suite already
// covers.
//
// (a) fb-vs-canvas equality: the sender (0,0) broadcasts "HELLO BRAMBLE" over
//     a real mesh (emu-channel-delivery.json); a receiver renders it. We
//     capture the raw wire "device_fb" event off the actual WebSocket
//     (wsCapture.ts, independent of the app's useSimulation.ts), decode it
//     with a from-scratch decoder (fbWire.ts, independent of the app's
//     framebuffer.ts), and independently verify "HELLO BRAMBLE" is really in
//     those bits using a from-scratch font-glyph search (glyphMatch.ts,
//     transcribed straight from font_6x8.h, independent of gosim's own
//     screen_assert.go). Then we read back the ACTUAL canvas pixels the
//     browser painted (canvasRead.ts) and assert they are pixel-for-pixel
//     identical to the wire decode. A decode, orientation, or bit-order bug
//     anywhere in the app's pipeline (framebuffer.ts's bit walk, Epaper.tsx's
//     putImageData, a transposed x/y) would make this comparison fail, since
//     none of the reference code is shared with the app.
//
// (b) full-refresh flash sequence: REMOVED from this suite. It sampled the
//     canvas on the wall clock and asserted the first painted sample was the
//     black flash fill, which loses a scheduling race on CPU-limited CI
//     runner pods (see the note at the end of this file for the full
//     rationale and the redesign a reliable version needs).

import { test, expect } from '@playwright/test';
import * as path from 'node:path';
import { decodeFbWire, gridEquals, diffCount } from '../lib/fbWire';
import { findText } from '../lib/glyphMatch';
import { readCanvasGrid, canvasSelector } from '../lib/canvasRead';
import { attachWsCapture, waitFor, type WsCapture } from '../lib/wsCapture';
import { loadScenario } from '../lib/uiActions';

const CHANNEL_TEXT = 'HELLO BRAMBLE';
const ARTIFACT_DIR = path.join(__dirname, '..', 'artifacts');

test.describe('display correctness', () => {
  let cap: WsCapture;

  test.beforeEach(async ({ page }) => {
    cap = attachWsCapture(page);
    await page.goto('/');
    await expect(page.getByTestId('view-tab-mesh')).toBeVisible();
    await page.getByTestId('view-tab-devices').click();
  });

  test('canvas pixels equal the wire framebuffer for a rendered channel message', async ({ page }) => {
    await loadScenario(page, 'emu-channel-delivery');

    // Ground truth off the wire: wait until SOME node's raw fb bytes really
    // contain "HELLO BRAMBLE" per an independent glyph search. This does not
    // touch the DOM at all, so it cannot be fooled by a UI decode bug -- it
    // proves delivery happened at the protocol level.
    const hit = await waitFor(
      () => {
        for (const ev of [...cap.fbEvents].reverse()) {
          const grid = decodeFbWire(ev.fb);
          if (findText(grid, CHANNEL_TEXT, 1).found) return ev;
        }
        return undefined;
      },
      // Covers the scenario's full send schedule (sender t=12s..100s); see
      // the note in functionality.spec.ts's delivery step. Event-driven.
      { timeoutMs: 150_000, label: `"${CHANNEL_TEXT}" in some node's wire fb` },
    );

    // Let that node's card exist and the paint schedule for this exact seq
    // settle (full refresh busy window is up to ~2.6s; partial ~300ms -- the
    // 2.6s here is epaperModel.ts's UI-only fallback default, EPD_MODEL.
    // defaultFullBusyMs, used only when no live busy_ms rides the frame; the
    // real firmware constant is SSD1680_BUSY_MS_FULL = 3000ms, see the flash
    // test below, and the live wire value is what that test actually waits
    // on via fullEv.busyMs).
    await expect(page.locator(canvasSelector(hit.node))).toBeVisible({ timeout: 10_000 });

    const wireGrid = decodeFbWire(hit.fb);

    // Poll the canvas until its independent glyph search ALSO finds the text
    // (bounded wait for the paint animation to settle onto this exact frame;
    // not a wait for network delivery, which is already done above).
    const canvasHit = await waitFor(
      async () => {
        const grid = await readCanvasGrid(page, hit.node);
        const found = findText(grid, CHANNEL_TEXT, 1);
        return found.found ? { grid, found } : undefined;
      },
      { timeoutMs: 20_000, intervalMs: 200, label: 'canvas to render the delivered text' },
    );

    expect(canvasHit.found.found, 'independent glyph search must find the text on the canvas').toBe(true);

    // Strict pixel-exact equality: the canvas's ink/paper classification
    // must match the wire decode at every one of the 250x122 pixels. This is
    // the assertion a decode/orientation/inversion bug is guaranteed to
    // break (a transposed axis or reversed bit order shreds this almost
    // everywhere, not just near the glyphs).
    const equal = gridEquals(wireGrid, canvasHit.grid);
    if (!equal) {
      await page.screenshot({ path: path.join(ARTIFACT_DIR, 'display-correctness-FAILURE.png') });
    }
    expect(equal, `wire fb and canvas readback differ at ${diffCount(wireGrid, canvasHit.grid)} of ${250 * 122} pixels`).toBe(true);

    await page.screenshot({ path: path.join(ARTIFACT_DIR, 'display-correctness-pass.png') });
  });

  // REMOVED: the 'full refresh plays the inversion flash, black first' test.
  //
  // It sampled the canvas from the test process on the wall clock and asserted
  // the FIRST painted sample it caught was the black flash fill, i.e. that the
  // sampler's first CDP canvas readback landed inside the opening ~busy/2
  // (~1.5s) of a 3s paint schedule. On the CPU-limited CI runner pods that
  // race is lost intermittently (observed: first painted sample 'white' or
  // 'mixed' when the pod is starved), so the assertion is not required-grade:
  // it fails on scheduling luck, not on flash-order bugs. External wall-clock
  // sampling cannot be made reliable here; a trustworthy version needs the UI
  // to expose its applied paint sequence (e.g. Epaper.tsx recording each
  // black/white/content application to a per-node test-visible log) so the
  // test can assert on recorded ORDER, event-driven, with no race. Bring the
  // test back with that redesign; do not reinstate the sampling version.
});
