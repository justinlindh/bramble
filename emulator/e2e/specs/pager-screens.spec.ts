// pager-screens.spec.ts
//
// Drives a single Virtual Pager firmware node (SSD1680 250x122 e-paper
// profile) through each primary text-UI screen and saves a per-screen PNG of
// the panel. It is the e-paper analogue of heltec-oled.spec.ts (128x64 OLED):
// it captures the ACTUAL firmware framebuffer the app painted onto the
// <canvas data-testid="epaper-canvas"> backing store (the same 250x122 bits
// render_screen() drew via display_draw_text), not a UI reimagining of the
// screens. Ink polarity (e-paper ink is dark, unlike the OLED's bright
// foreground) is handled by lib/canvasRead's toGrid, so the same glyph search
// finds the header text on either panel.
//
// Navigation is button-driven, exactly as on hardware: the text UI cycles
// screens on a "down" press (ui_manager.c). The pager is a non-keyboard board,
// so its Compose slot forks to Stats, giving the same ring the Heltec walks:
//   Main -> Messages -> Nodes -> Stats -> GPS -> Settings
// and Settings traps "down" (it steps the settings cursor instead of leaving),
// so the walk visits Settings last and never presses past it. Each step is
// gated on the target screen's header text actually appearing on the panel (an
// independent glyph search over the canvas readback), never on a sleep, so the
// capture is deterministic.
//
// This spec self-skips when the pager node binary is absent, so it never
// breaks a partial checkout. Build it first with `make -C emulator node`,
// which produces emulator/node/build/bramble-node.elf (the path
// emu-pager-screens.json references).

import { test, expect } from '@playwright/test';
import * as fs from 'node:fs';
import * as path from 'node:path';
import type { Page } from '@playwright/test';
import { findText } from '../lib/glyphMatch';
import { readCanvasGrid, canvasSelector } from '../lib/canvasRead';
import { loadScenario, clickButton } from '../lib/uiActions';

const REPO_ROOT = path.resolve(__dirname, '..', '..', '..');
const PAGER_BIN = path.join(REPO_ROOT, 'emulator', 'node', 'build', 'bramble-node.elf');
const ARTIFACT_DIR = path.join(__dirname, '..', 'artifacts', 'pager-screens');

// Captures the panel to PNG at exact panel resolution and at a 6x
// nearest-neighbor upscale (for human viewing). Both come straight off the
// canvas backing store, so they are pixel-exact, not a screenshot of the
// CSS-scaled element.
async function capturePngs(page: Page, nodeId: string, name: string): Promise<void> {
  const shots = await page.$eval(canvasSelector(nodeId), (canvas: HTMLCanvasElement) => {
    const exact = canvas.toDataURL('image/png');
    const scale = 6;
    const up = document.createElement('canvas');
    up.width = canvas.width * scale;
    up.height = canvas.height * scale;
    const uctx = up.getContext('2d');
    if (!uctx) throw new Error('capturePngs: no 2d context');
    uctx.imageSmoothingEnabled = false;
    uctx.drawImage(canvas, 0, 0, up.width, up.height);
    return { exact, up: up.toDataURL('image/png') };
  });
  const write = (dataUrl: string, suffix: string) => {
    const b64 = dataUrl.replace(/^data:image\/png;base64,/, '');
    fs.writeFileSync(path.join(ARTIFACT_DIR, `${name}${suffix}.png`), Buffer.from(b64, 'base64'));
  };
  write(shots.exact, '');
  write(shots.up, '@6x');
}

// The screen ring in walk order, each with a header string that uniquely
// identifies it once rendered. "Bramble" is the Main header; the rest are the
// literal headers from render_screen() (Compose forks to "Stats" on a
// non-keyboard board like the pager).
const SCREENS: { name: string; anchor: string; content?: string[] }[] = [
  { name: 'main', anchor: 'Bramble' },
  { name: 'messages', anchor: 'Messages', content: ['HELLO'] },
  // Either named neighbor proves beacons carried a name onto the panel;
  // which of the two wins the contended discovery window is not
  // deterministic under the real collision/half-duplex ether model.
  { name: 'nodes', anchor: 'Nodes', content: ['Bob', 'Carol'] },
  { name: 'stats', anchor: 'Stats' },
  { name: 'gps', anchor: 'GPS' },
  { name: 'settings', anchor: 'Settings' },
];

// Returns the header currently on the panel, or '(none)'. Checks 'Settings'
// BEFORE 'GPS': the Settings screen contains the literal row "GPS: On", so a
// GPS-first search misreads a rendered Settings screen as GPS.
async function currentHeader(page: Page, nodeId: string): Promise<string> {
  const grid = await readCanvasGrid(page, nodeId);
  for (const h of ['Bramble', 'Messages', 'Nodes', 'Stats', 'Settings', 'GPS']) {
    if (findText(grid, h, 1).found) return h;
  }
  return '(none)';
}

// Bring the panel to Main deterministically before the walk. Two pager-specific
// behaviors make this necessary (both real device behavior, main.c):
//  1. Alert acknowledge: the scenario's incoming traffic arms the message
//     alert (BOARD_CAP_ALERTS), and while it is unconfirmed the FIRST button
//     press only acknowledges it, consumed, no navigation. Whether it is
//     still armed at walk time is timing-dependent, so the spec cannot
//     assume either state.
//  2. Message auto-switch: traffic may have parked the panel on Messages (the
//     30s auto-restore may or may not have fired yet by walk time).
// Strategy: press 'down' until the header CHANGES (at most one press is ever
// consumed by the alert, so two presses suffice), which both consumes any
// pending alert and proves navigation works; then walk back up to Main with
// header-guarded 'up' presses. From the only possible start screens (Main or
// auto-switched Messages) this path never touches Settings, whose cursor
// traps up/down (only a double-press exits it).
async function ackAlertAndReturnToMain(page: Page, nodeId: string): Promise<void> {
  const start = await currentHeader(page, nodeId);
  if (start !== 'Bramble' && start !== 'Messages') {
    throw new Error(`ackAlertAndReturnToMain: unexpected start screen "${start}"`);
  }
  let cur = start;
  for (let i = 0; i < 2 && cur === start; i++) {
    await clickButton(page, nodeId, 'down');
    await page.waitForTimeout(800);
    cur = await currentHeader(page, nodeId);
  }
  if (cur === start) {
    throw new Error(`ackAlertAndReturnToMain: two down presses never left "${start}"`);
  }
  for (let i = 0; i < 3 && cur !== 'Bramble'; i++) {
    await clickButton(page, nodeId, 'up');
    await page.waitForTimeout(800);
    cur = await currentHeader(page, nodeId);
  }
  if (cur !== 'Bramble') {
    throw new Error(`ackAlertAndReturnToMain: never reached Main (stuck on "${cur}")`);
  }
}

async function waitForAnyAnchor(page: Page, nodeId: string, anchors: string[], timeoutMs: number): Promise<void> {
  const deadline = Date.now() + timeoutMs;
  let lastErr = '';
  while (Date.now() < deadline) {
    try {
      const grid = await readCanvasGrid(page, nodeId);
      for (const a of anchors) {
        if (findText(grid, a, 1).found) return;
      }
      lastErr = `none of [${anchors.join(', ')}] on panel yet`;
    } catch (e) {
      lastErr = String(e);
    }
    await page.waitForTimeout(150);
  }
  throw new Error(`waitForAnyAnchor: timed out after ${timeoutMs}ms (${lastErr})`);
}

async function waitForAnchor(page: Page, nodeId: string, anchor: string, timeoutMs: number): Promise<void> {
  const deadline = Date.now() + timeoutMs;
  let lastErr = '';
  while (Date.now() < deadline) {
    try {
      const grid = await readCanvasGrid(page, nodeId);
      if (findText(grid, anchor, 1).found) return;
      lastErr = `"${anchor}" not yet on panel`;
    } catch (e) {
      lastErr = String(e);
    }
    await page.waitForTimeout(150);
  }
  throw new Error(`waitForAnchor: timed out after ${timeoutMs}ms (${lastErr})`);
}

test.describe('pager e-paper per-screen capture', () => {
  test.skip(!fs.existsSync(PAGER_BIN), `Pager node binary missing: ${PAGER_BIN} (build it: make -C emulator node)`);

  test.beforeAll(() => {
    fs.mkdirSync(ARTIFACT_DIR, { recursive: true });
  });

  test('captures Main, Messages, Nodes, Stats, GPS, Settings on the 250x122 panel', async ({ page }) => {
    await page.goto('/');
    await expect(page.getByTestId('view-tab-mesh')).toBeVisible();
    await page.getByTestId('view-tab-devices').click();

    await loadScenario(page, 'emu-pager-screens');

    // Resolve the pager: the only e-paper node in the scenario (its neighbors
    // are OLED, so only the pager's card carries an epaper-canvas). Pick that
    // card rather than the first, which may be an OLED neighbor.
    const card = page
      .locator('[data-testid^="device-card-"]:has(canvas[data-testid="epaper-canvas"])')
      .first();
    await expect(card).toBeVisible({ timeout: 30_000 });
    const testId = await card.getAttribute('data-testid');
    const nodeId = (testId ?? '').replace('device-card-', '');
    expect(nodeId, 'resolved a firmware node id from the device card').not.toBe('');

    // The e-paper canvas must exist and have painted the boot Main screen.
    await expect(page.locator(canvasSelector(nodeId))).toBeVisible({ timeout: 20_000 });

    // Sanity-check panel geometry: the backing store is exactly 250x122.
    const dims = await page.$eval(canvasSelector(nodeId), (c: HTMLCanvasElement) => ({ w: c.width, h: c.height }));
    expect(dims).toEqual({ w: 250, h: 122 });

    // Let neighbor discovery and the traffic burst settle so the Nodes and
    // Messages screens have real content before the walk (the neighbors beacon
    // and send inside the first ~16s). The walk presses a button at every step,
    // keeping the UI idle timer below the message-auto-switch threshold, so no
    // late arrival yanks the screen mid-walk.
    await page.waitForTimeout(28_000);

    // Acknowledge the pending message alert and settle on Main (see the
    // helper's comment for why the first press never navigates).
    await ackAlertAndReturnToMain(page, nodeId);

    // Walk the ring, gating each capture on the target screen's header and, for
    // the content screens, on the real peer/message text being on the panel.
    for (let i = 0; i < SCREENS.length; i++) {
      const s = SCREENS[i];
      if (i > 0) await clickButton(page, nodeId, 'down');
      // Boot splash + GPS acquisition can lag; a generous, event-driven wait.
      await waitForAnchor(page, nodeId, s.anchor, 30_000);
      if (s.content) await waitForAnyAnchor(page, nodeId, s.content, 30_000);
      await capturePngs(page, nodeId, s.name);
    }

    // Every screen produced both an exact and an upscaled PNG.
    for (const s of SCREENS) {
      expect(fs.existsSync(path.join(ARTIFACT_DIR, `${s.name}.png`)), `${s.name}.png written`).toBe(true);
      expect(fs.existsSync(path.join(ARTIFACT_DIR, `${s.name}@6x.png`)), `${s.name}@6x.png written`).toBe(true);
    }
  });
});
