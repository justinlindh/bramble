// functionality.spec.ts
//
// Archived emulator plan (docs/archive/plans/emulator-plan.md) Task 13,
// deliverable 3: basic FUNCTIONALITY through the real UI --
// device cards, boot screen, button input, reset persistence, and the
// headline end-to-end message (real firmware A sends, real firmware B
// receives and renders, the browser shows it) -- plus the two no-regression
// sentinels (split-identity card count, mesh map still renders).
//
// One scenario load (emu-channel-delivery: 1 sender + 2 receivers = 3
// firmware nodes) carries steps (a)-(e) as test.step()s within a single test,
// rather than one scenario load per assertion: each load spawns three real
// IDF-linux firmware processes, and re-paying that cost per assertion would
// blow the suite's 4-minute wall-clock budget for no isolation benefit (every
// step here reads state, it never invalidates a later step's preconditions).

import { test, expect } from '@playwright/test';
import * as path from 'node:path';
import { decodeFbWire } from '../lib/fbWire';
import { findText } from '../lib/glyphMatch';
import { readCanvasGrid, canvasSelector } from '../lib/canvasRead';
import { attachWsCapture, waitFor, type WsCapture } from '../lib/wsCapture';
import { loadScenario, clickButton, holdReset } from '../lib/uiActions';

const ARTIFACT_DIR = path.join(__dirname, '..', 'artifacts');

test('mesh map still renders (no regression)', async ({ page }) => {
  await page.goto('/');
  await expect(page.getByTestId('view-tab-mesh')).toBeVisible();
  // Default view is the mesh map; no scenario needed for it to render.
  await expect(page.getByTestId('mesh-canvas')).toBeVisible();
  await expect(page.getByTestId('mesh-canvas').locator('svg')).toBeVisible();
});

test('device cards, boot screen, buttons, reset persistence, and message delivery', async ({ page }) => {
  const cap: WsCapture = attachWsCapture(page);
  await page.goto('/');
  await page.getByTestId('view-tab-devices').click();
  await loadScenario(page, 'emu-channel-delivery');

  let nodeIds: string[] = [];

  await test.step('(a) exactly N device cards for N firmware nodes (split-identity sentinel)', async () => {
    // 3 firmware_nodes in emu-channel-delivery.json: 1 sender + 2 receivers.
    await waitFor(() => (cap.distinctNodes().length === 3 ? true : undefined), {
      timeoutMs: 30_000,
      label: '3 distinct node_joined attaches',
    });
    nodeIds = cap.distinctNodes();
    expect(nodeIds, 'distinct hello ids from node_joined').toHaveLength(3);

    const cards = page.locator('[data-testid^="device-card-"]');
    await expect(cards).toHaveCount(3, { timeout: 10_000 });

    // Every wire-level id has exactly one DOM card, and vice versa -- the
    // regression this guards is a split identity (one node id spawning two
    // separate device cards, or wire and DOM disagreeing on node count).
    for (const id of nodeIds) {
      await expect(page.getByTestId(`device-card-${id}`)).toHaveCount(1);
    }
  });

  await test.step('(b) boot screen text ("BRAMBLE") appears', async () => {
    const bootHit = await waitFor(
      () => {
        for (const ev of cap.fbEvents) {
          if (ev.kind !== 'full') continue;
          const grid = decodeFbWire(ev.fb);
          if (findText(grid, 'BRAMBLE', 2).found) return ev;
        }
        return undefined;
      },
      { timeoutMs: 20_000, label: 'boot screen "BRAMBLE" splash in a wire fb' },
    );
    // Cross-check it is actually visible on that node's canvas too, not just
    // in the wire bytes (a stronger pixel-level proof is display-correctness.spec.ts).
    const canvasFound = await waitFor(
      async () => {
        const grid = await readCanvasGrid(page, bootHit.node);
        return findText(grid, 'BRAMBLE', 2).found || undefined;
      },
      // Canvas readback is CDP round-trips into a chromium that shares the CI
      // pod's CPU; 8s expired on a contended pod after the wire hit had
      // already proven the render. Event-driven, so fast boxes exit early.
      { timeoutMs: 20_000, intervalMs: 200, label: 'boot text visible on canvas' },
    );
    expect(canvasFound).toBe(true);
  });

  await page.screenshot({ path: path.join(ARTIFACT_DIR, 'functionality-device-view.png'), fullPage: true });

  await test.step('(c) UP/DOWN/SELECT clicks emit the wire btn frame and a visible firmware reaction', async () => {
    const target = nodeIds[0];
    for (const id of ['up', 'down', 'select'] as const) {
      const consoleBefore = cap.consoleEvents.filter((e) => e.node === target && e.line.includes('Button event:')).length;
      const sentBefore = cap.sentBtn.length;

      await clickButton(page, target, id);

      // Outgoing wire frame: { type:"btn", node, id, edge } for both the
      // press and release edges (useSimulation.ts's sendButton contract).
      await waitFor(
        () =>
          cap.sentBtn
            .slice(sentBefore)
            .some((f) => f.node === target && f.id === id && f.edge === 'down') || undefined,
        { timeoutMs: 15_000, label: `outgoing btn frame for ${id} down` },
      );
      await waitFor(
        () =>
          cap.sentBtn
            .slice(sentBefore)
            .some((f) => f.node === target && f.id === id && f.edge === 'up') || undefined,
        { timeoutMs: 15_000, label: `outgoing btn frame for ${id} up` },
      );

      // Visible firmware reaction: main.c logs `Button event: %d` on every
      // dispatched press (ui_button_t via button_poll), forwarded to the
      // browser as a console line for that node.
      await waitFor(
        () => {
          const now = cap.consoleEvents.filter((e) => e.node === target && e.line.includes('Button event:')).length;
          return now > consoleBefore || undefined;
        },
        { timeoutMs: 15_000, label: `console reaction to ${id} press` },
      );
    }
  });

  await test.step('(e) a channel message sent by node A renders on node B\'s e-paper canvas', async () => {
    // Mirrors emu-channel-delivery.json's own acceptance bar (run_scenarios.sh
    // gates on `screen-assert -min-nodes 2 -text "HELLO BRAMBLE"`): at least 2
    // distinct nodes must render the broadcast text. This is the browser-level
    // proof that real firmware A transmitted and real firmware B received and
    // painted it; display-correctness.spec.ts covers the strict pixel-exact
    // proof for one such node, so this step stays a presence check across
    // multiple nodes rather than repeating that full comparison.
    const hits = await waitFor(
      () => {
        const rendered = new Set<string>();
        for (const ev of cap.fbEvents) {
          if (rendered.has(ev.node)) continue;
          const grid = decodeFbWire(ev.fb);
          if (findText(grid, 'HELLO BRAMBLE', 1).found) rendered.add(ev.node);
        }
        return rendered.size >= 2 ? rendered : undefined;
      },
      // The budget covers the scenario's FULL send schedule (12 broadcasts,
      // sender t=12s..100s), not just the first sends: on a CPU-contended CI
      // pod (this suite shares the pod with headless chromium) the early
      // sends can straggle, and a 40s window that opens ~t=15s used to close
      // before the later sends landed, failing runs whose delivery was fine.
      // waitFor is event-driven, so a fast box still exits in seconds.
      { timeoutMs: 150_000, label: '>=2 distinct nodes rendering "HELLO BRAMBLE" on the wire' },
    );
    expect(hits.size, 'distinct nodes with the message in their wire fb').toBeGreaterThanOrEqual(2);

    // Spot-check one of them on the actual canvas (the browser really painted it).
    const [oneNode] = hits;
    await waitFor(
      async () => {
        const grid = await readCanvasGrid(page, oneNode);
        return findText(grid, 'HELLO BRAMBLE', 1).found || undefined;
      },
      { timeoutMs: 20_000, intervalMs: 200, label: `canvas render of the message on node ${oneNode}` },
    );
  });

  // Reset runs LAST, after the message-delivery assertion above: rebooting a
  // receiver mid-scenario is a real, valid thing to exercise, but doing it
  // before checking delivery would make that check race a node coming back up
  // instead of testing message delivery on its own.
  await test.step('(d) RESET (hold-to-confirm) restarts the node and the SAME identity reattaches', async () => {
    const resetTarget = nodeIds[nodeIds.length - 1];
    const attachesBefore = cap.joinEvents.filter((e) => e.node === resetTarget).length;

    await holdReset(page, resetTarget);

    // The synthetic reset fires both edges once the hold clears
    // PagerDevice.tsx's RESET_HOLD_MS threshold (button_virt.c: a "reset" btn
    // message is not a UI event, it's exit(0) -- the supervisor restarts the
    // process, and a persisted identity in flash.bin re-attaches with the
    // SAME hello id, exactly like emulator/scripts/smoke_live.sh's
    // persistence check, but observed here through the live browser session).
    await waitFor(
      () => cap.sentBtn.some((f) => f.node === resetTarget && f.id === 'reset') || undefined,
      { timeoutMs: 15_000, label: 'outgoing reset btn frame' },
    );
    await waitFor(
      () => (cap.joinEvents.filter((e) => e.node === resetTarget).length > attachesBefore ? true : undefined),
      { timeoutMs: 30_000, label: `node ${resetTarget} to re-attach with its original id` },
    );

    // Identity persisted (not a fresh id) and no phantom card was left behind.
    expect(cap.distinctNodes(), 'still exactly the same 3 identities after a reset/reattach').toHaveLength(3);
    await expect(page.locator('[data-testid^="device-card-"]')).toHaveCount(3, { timeout: 10_000 });
    await expect(page.getByTestId(`device-card-${resetTarget}`)).toHaveCount(1);
  });
});
