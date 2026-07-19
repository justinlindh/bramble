// heltec-oled.spec.ts
//
// Drives a Virtual Heltec firmware node (SSD1306 128x64 OLED profile) through
// each primary text-UI screen and saves a per-screen PNG of the panel. It is
// the OLED analogue of display-correctness.spec.ts (e-paper): it captures the
// ACTUAL firmware framebuffer the app painted onto the <canvas data-testid=
// "oled-canvas"> backing store (the same 128x64 bytes render_screen() drew via
// display_draw_text), not a UI reimagining of the screens.
//
// Navigation is button-driven, exactly as on hardware: the text UI cycles
// screens on a "down" press (ui_manager.c). The ring is
//   Main -> Messages -> Nodes -> Stats (Compose slot, board-forked) -> GPS ->
//   Settings
// and Settings traps "down" (it steps the settings cursor instead of leaving),
// so the walk visits Settings last and never presses past it. Each step is
// gated on the target screen's header text actually appearing on the panel (an
// independent glyph search over the canvas readback), never on a sleep, so the
// capture is deterministic.
//
// This spec self-skips when the Heltec node binary is absent, so it never
// breaks the existing e2e run (which builds only the e-paper pager node). Build
// it first with emulator/scripts/build_node_heltec.sh, which produces
// emulator/node/build-heltec/bramble-node.elf (the path emu-heltec-oled.json
// references).

import { test, expect } from '@playwright/test';
import * as fs from 'node:fs';
import * as path from 'node:path';
import type { Page } from '@playwright/test';
import { findText } from '../lib/glyphMatch';
import type { BitGrid } from '../lib/fbWire';
import { loadScenario, clickButton } from '../lib/uiActions';

const REPO_ROOT = path.resolve(__dirname, '..', '..', '..');
const HELTEC_BIN = path.join(REPO_ROOT, 'emulator', 'node', 'build-heltec', 'bramble-node.elf');
const ARTIFACT_DIR = path.join(__dirname, '..', 'artifacts', 'heltec-oled');

// Panel foreground threshold: a lit OLED pixel is near-white (Oled.tsx LIT
// red = 0xe6), an unlit one near-black (OFF red = 0x06). 128 splits them.
const LIT_THRESHOLD = 128;

function oledSelector(nodeId: string): string {
  return `[data-testid="device-card-${nodeId}"] canvas[data-testid="oled-canvas"]`;
}

// Reads the canvas backing store and classifies each pixel as lit (foreground,
// true) or dark. Returns a [y][x] BitGrid so glyphMatch.findText can search it,
// exactly as canvasRead.ts does for the e-paper panel (only the polarity of
// "inked" differs: OLED foreground is bright, e-paper ink is dark).
async function readOledGrid(page: Page, nodeId: string): Promise<BitGrid> {
  const { data, w, h } = await page.$eval(oledSelector(nodeId), (canvas: HTMLCanvasElement) => {
    const ctx = canvas.getContext('2d');
    if (!ctx) throw new Error('readOledGrid: no 2d context');
    const img = ctx.getImageData(0, 0, canvas.width, canvas.height);
    return { data: Array.from(img.data), w: canvas.width, h: canvas.height };
  });
  const grid: BitGrid = [];
  for (let y = 0; y < h; y++) {
    const row: boolean[] = new Array(w);
    for (let x = 0; x < w; x++) row[x] = data[(y * w + x) * 4] > LIT_THRESHOLD;
    grid.push(row);
  }
  return grid;
}

// Captures the panel to PNG at exact panel resolution and at a 6x
// nearest-neighbor upscale (for human viewing). Both come straight off the
// canvas backing store, so they are pixel-exact, not a screenshot of the
// CSS-scaled element.
async function capturePngs(page: Page, nodeId: string, name: string): Promise<void> {
  const shots = await page.$eval(oledSelector(nodeId), (canvas: HTMLCanvasElement) => {
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
// non-keyboard board like the Heltec).
const SCREENS: { name: string; anchor: string }[] = [
  { name: 'main', anchor: 'Bramble' },
  { name: 'messages', anchor: 'Messages' },
  { name: 'nodes', anchor: 'Nodes' },
  { name: 'stats', anchor: 'Stats' },
  { name: 'gps', anchor: 'GPS' },
  { name: 'settings', anchor: 'Settings' },
];

async function waitForAnchor(page: Page, nodeId: string, anchor: string, timeoutMs: number): Promise<void> {
  const deadline = Date.now() + timeoutMs;
  let lastErr = '';
  while (Date.now() < deadline) {
    try {
      const grid = await readOledGrid(page, nodeId);
      if (findText(grid, anchor, 1).found) return;
      lastErr = `"${anchor}" not yet on panel`;
    } catch (e) {
      lastErr = String(e);
    }
    await page.waitForTimeout(150);
  }
  throw new Error(`waitForAnchor: timed out after ${timeoutMs}ms (${lastErr})`);
}

test.describe('heltec OLED per-screen capture', () => {
  test.skip(!fs.existsSync(HELTEC_BIN), `Heltec node binary missing: ${HELTEC_BIN} (build it: emulator/scripts/build_node_heltec.sh)`);

  test.beforeAll(() => {
    fs.mkdirSync(ARTIFACT_DIR, { recursive: true });
  });

  test('captures Main, Messages, Nodes, Stats, GPS, Settings on the 128x64 panel', async ({ page }) => {
    await page.goto('/');
    await expect(page.getByTestId('view-tab-mesh')).toBeVisible();
    await page.getByTestId('view-tab-devices').click();

    await loadScenario(page, 'emu-heltec-oled');

    // Exactly one device card (single-node scenario). Wait for it to attach and
    // resolve its node id from the testid.
    const card = page.locator('[data-testid^="device-card-"]').first();
    await expect(card).toBeVisible({ timeout: 30_000 });
    const testId = await card.getAttribute('data-testid');
    const nodeId = (testId ?? '').replace('device-card-', '');
    expect(nodeId, 'resolved a firmware node id from the device card').not.toBe('');

    // The OLED canvas must exist and have painted the boot Main screen.
    await expect(page.locator(oledSelector(nodeId))).toBeVisible({ timeout: 20_000 });

    // Sanity-check panel geometry: the backing store is exactly 128x64.
    const dims = await page.$eval(oledSelector(nodeId), (c: HTMLCanvasElement) => ({ w: c.width, h: c.height }));
    expect(dims).toEqual({ w: 128, h: 64 });

    // Walk the ring, gating each capture on the target screen's header.
    for (let i = 0; i < SCREENS.length; i++) {
      const s = SCREENS[i];
      if (i > 0) await clickButton(page, nodeId, 'down');
      // Boot splash + GPS acquisition can lag; a generous, event-driven wait.
      await waitForAnchor(page, nodeId, s.anchor, 30_000);
      await capturePngs(page, nodeId, s.name);
    }

    // Every screen produced both an exact and an upscaled PNG.
    for (const s of SCREENS) {
      expect(fs.existsSync(path.join(ARTIFACT_DIR, `${s.name}.png`)), `${s.name}.png written`).toBe(true);
      expect(fs.existsSync(path.join(ARTIFACT_DIR, `${s.name}@6x.png`)), `${s.name}@6x.png written`).toBe(true);
    }
  });
});
