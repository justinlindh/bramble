// playground-tour.spec.ts
//
// Browser coverage of the emulator playground: the guided tour overlay
// (simulator/ui/src/tour) driving the emu-playground fleet of three real
// firmware processes through the whole first-contact story.
//
// Every assertion here is made twice over, from two independent places: the
// app's own tour state (which step it is on, what it says it observed) and the
// raw broker WebSocket frames (wsCapture.ts), which are the firmware's actual
// console output before the app has touched it. A tour that advanced without
// the mesh doing the work would pass the first and fail the second.
//
// It also writes the two screenshots docs/playground.md embeds, so the images
// in the docs are captures of this exact run rather than something copied in
// by hand and left to drift.
//
// The scenario boots UNPROVISIONED on purpose, so nothing at all happens until
// the tour provisions the fleet: there is no auto-send anywhere in this
// scenario, and every message on the ether below is one the tour originated.
// The control plane a keyed node runs by itself (beacons, its own attestation
// cadence) is the firmware's, unchanged for the playground.

import { test, expect, type Page } from '@playwright/test';
import * as fs from 'node:fs';
import * as path from 'node:path';
import { findText, renderGlyphs, type GlyphPattern } from '../lib/glyphMatch';
import { readCanvasGrid } from '../lib/canvasRead';
import type { BitGrid } from '../lib/fbWire';
import { attachWsCapture, waitFor, type WsCapture } from '../lib/wsCapture';
import { clickButton, loadScenarioNoPlay } from '../lib/uiActions';

const REPO_ROOT = path.resolve(__dirname, '..', '..', '..');
const NODE_BIN = path.join(REPO_ROOT, 'emulator', 'node', 'build', 'bramble-node.elf');
const DOC_IMAGES = path.join(REPO_ROOT, 'docs', 'images');

// Slot positions from simulator/scenarios/emu-playground.json. The line is the
// point of the scenario: 150-unit range means ALPHA and CHARLIE (200 apart)
// can only reach each other through BRAVO.
const SLOT = { alpha: 0, bravo: 100, charlie: 200 } as const;

// Texts the tour sends (simulator/ui/src/tour/fleet.ts). Duplicated rather
// than imported: the spec lives outside simulator/ui and asserting on a
// literal keeps this a black-box check of what actually crossed the ether.
const CHANNEL_TEXT = 'HELLO PLAYGROUND';
const DM_TEXT = 'DM FOR BRAVO';

// Budgets, sized as starvation multiples rather than as durations. Every wait
// below is event-driven and a healthy box clears each step in seconds, so
// these only ever elapse on a genuine regression: what they have to survive is
// a contended runner where the firmware's real-time schedule stretches, and
// the repo's own DM gate (emulator/ci/run_scenarios.sh's dm_suite) budgets a
// ~90 s schedule at 1230 s for exactly that reason. The key-exchange step gets
// the widest margin because it is the one that has been observed timing out
// when the host was busy; the cost of the headroom on a healthy box is nothing,
// since the wait ends the moment the mesh delivers.
const ATTACH_MS = 90_000;
const PROVISION_MS = 90_000;
const DELIVERY_MS = 300_000;
const SESSION_MS = 600_000;
const RECEIPT_MS = 420_000;

interface Fleet {
  alpha: string;
  bravo: string;
  charlie: string;
}

function consoleHas(cap: WsCapture, node: string, needle: string): boolean {
  return cap.consoleEvents.some((e) => e.node === node && e.line.includes(needle));
}

function provisionedNodes(cap: WsCapture): string[] {
  return [
    ...new Set(
      cap.consoleEvents
        .filter((e) => e.line.includes('network key provisioned over emu-link'))
        .map((e) => e.node),
    ),
  ];
}

// Delivery receipts as the ORIGINATOR logged them, with the relay path they
// came home by (mesh_reliability.c handle_delivery_receipt).
function receiptsAt(cap: WsCapture, node: string): { from: string; path: string[] }[] {
  const re =
    /Delivery receipt from ([0-9A-F]{8}) for broadcast [0-9A-F]{8} \(\d+ relay hop\(s\)(?: via ([0-9A-F>]+))?\)/;
  const out: { from: string; path: string[] }[] = [];
  for (const e of cap.consoleEvents) {
    if (e.node !== node) continue;
    const m = re.exec(e.line);
    if (m) out.push({ from: m[1], path: m[2] ? m[2].split('>') : [] });
  }
  return out;
}

// --- reading the safety number off the panel --------------------------
//
// The 7-digit safety number is the one thing on the peer-detail screen drawn
// in the large font (main.c's display_draw_text_large, grouped 3 + 4 by
// sas_format_grouped). Its value is derived from both identities and is not
// knowable from outside, so the spec reconstructs it a cell at a time: anchor
// on any large digit, then read the eight cells that follow from that exact
// offset. That is stricter than searching for a shape, because a wrong anchor
// simply fails to extend and the next one is tried.

const LARGE_CELL_W = 12; // 6px font at scale 2, matching renderGlyphs
const LARGE_CELL_H = 16; // 8px font at scale 2

function matchAt(grid: BitGrid, pattern: GlyphPattern, ox: number, oy: number): boolean {
  for (let px = 0; px < pattern.length; px++) {
    for (let py = 0; py < pattern[px].length; py++) {
      if ((grid[oy + py]?.[ox + px] ?? false) !== pattern[px][py]) return false;
    }
  }
  return true;
}

function anchorsFor(grid: BitGrid, ch: string): { x: number; y: number }[] {
  const { pattern, ok } = renderGlyphs(ch, 2);
  if (!ok) return [];
  const out: { x: number; y: number }[] = [];
  const w = pattern.length;
  const h = pattern[0]?.length ?? 0;
  for (let y = 0; y + h <= grid.length; y++) {
    for (let x = 0; x + w <= (grid[y]?.length ?? 0); x++) {
      if (matchAt(grid, pattern, x, y)) out.push({ x, y });
    }
  }
  return out;
}

// readSafetyNumber returns the grouped safety number as painted ("DDD DDDD"),
// or null when the panel is not showing one yet.
function readSafetyNumber(grid: BitGrid): string | null {
  const digits = '0123456789';
  const cells: Record<string, GlyphPattern> = {};
  for (const ch of digits) {
    const { pattern, ok } = renderGlyphs(ch, 2);
    if (ok) cells[ch] = pattern;
  }
  // The separating space has no ink of its own, so renderGlyphs refuses it
  // (never treat "found nothing" as a match). Its cell is an empty box of the
  // same size, which is a real constraint: it rejects an anchor that landed on
  // a run of digits with nothing between them.
  const blank: GlyphPattern = Array.from({ length: LARGE_CELL_W }, () =>
    new Array(LARGE_CELL_H).fill(false),
  );

  for (const d of digits) {
    for (const anchor of anchorsFor(grid, d)) {
      let text = '';
      for (let i = 0; i < 8; i++) {
        const x = anchor.x + i * LARGE_CELL_W;
        if (i === 3) {
          if (!matchAt(grid, blank, x, anchor.y)) break;
          text += ' ';
          continue;
        }
        const hit = digits.split('').find((ch) => matchAt(grid, cells[ch], x, anchor.y));
        if (!hit) break;
        text += hit;
      }
      if (/^\d{3} \d{4}$/.test(text)) return text;
    }
  }
  return null;
}

async function shoot(page: Page, name: string): Promise<string> {
  fs.mkdirSync(DOC_IMAGES, { recursive: true });
  const file = path.join(DOC_IMAGES, name);
  await page.screenshot({ path: file });
  return file;
}

// step waits for the tour to be showing the named step. The overlay stamps it
// on the panel, which is also what the app's own auto-advance drives, so this
// is the app asserting it observed the previous step's milestone.
async function expectStep(page: Page, id: string, timeoutMs: number): Promise<void> {
  await expect(page.getByTestId('tour-panel')).toHaveAttribute('data-tour-step', id, {
    timeout: timeoutMs,
  });
}

// clickUntil presses a tour action button until `check` reports the mesh did
// the thing, re-pressing on a slow cadence.
//
// This is not a retry that hides a flaky assertion: it is what the protocol
// requires of the caller. A channel broadcast is Bramble's Broadcast tier,
// which has no acknowledgement and no retransmission by design (0 retries,
// components/reliability), so a frame that collides with another node's
// transmission is gone and the only recovery is for a person to send it
// again. The shipped emu-channel-delivery scenario handles the same physics
// the same way, with a 12-send burst. A genuine relaying regression still
// fails here: no number of presses ever produces the delivery, and the budget
// runs out.
async function clickUntil(
  page: Page,
  testid: string,
  check: () => boolean,
  opts: { timeoutMs: number; everyMs: number; label: string },
): Promise<void> {
  const deadline = Date.now() + opts.timeoutMs;
  for (;;) {
    await page.getByTestId(testid).click();
    const until = Math.min(Date.now() + opts.everyMs, deadline);
    while (Date.now() < until) {
      if (check()) return;
      await new Promise((r) => setTimeout(r, 250));
    }
    if (check()) return;
    if (Date.now() >= deadline) {
      throw new Error(`clickUntil: ${opts.timeoutMs}ms elapsed waiting for ${opts.label}`);
    }
  }
}

// pressUntil presses a face button until the panel shows `text`, so screen
// navigation is gated on what the firmware actually painted rather than on a
// press count that assumes which screen the device happened to be on.
async function pressUntil(
  page: Page,
  node: string,
  button: 'up' | 'down' | 'select',
  text: string,
  presses: number,
): Promise<void> {
  for (let i = 0; i < presses; i++) {
    const grid = await readCanvasGrid(page, node);
    if (findText(grid, text, 1).found) return;
    await clickButton(page, node, button);
    await page.waitForTimeout(1_200);
  }
  const grid = await readCanvasGrid(page, node);
  expect(findText(grid, text, 1).found, `"${text}" on ${node} after ${presses} ${button} presses`).toBe(
    true,
  );
}

test.describe('emulator playground', () => {
  test.skip(!fs.existsSync(NODE_BIN), `firmware node binary missing: ${NODE_BIN}`);

  test('the tour is dismissible and resumes where it was left', async ({ page }) => {
    await page.goto('/?tour=1');

    const panel = page.getByTestId('tour-panel');
    await expect(panel).toBeVisible();
    await expect(panel).toHaveAttribute('data-tour-step', 'orientation');
    await expect(page.getByTestId('tour-count')).toHaveText('Step 1 of 5');

    // Skipping is always available, so a visitor is never stuck behind a step
    // whose milestone their fleet has not reached.
    await page.getByTestId('tour-skip').click();
    await expect(panel).toHaveAttribute('data-tour-step', 'provision');

    // Dismiss collapses the tour to a resume affordance and leaves the app
    // itself untouched.
    await page.getByTestId('tour-dismiss').click();
    await expect(panel).toHaveCount(0);
    await expect(page.getByTestId('mesh-canvas')).toBeVisible();
    await page.getByTestId('tour-resume').click();
    await expect(panel).toHaveAttribute('data-tour-step', 'provision');

    // Resumable across a reload, which is what makes it usable at all: the
    // emulator page gets reloaded constantly.
    await page.reload();
    await expect(page.getByTestId('tour-panel')).toHaveAttribute('data-tour-step', 'provision');

    // And the tour stays off for the ordinary simulator UI.
    await page.goto('/?tour=0');
    await expect(page.getByTestId('tour-panel')).toHaveCount(0);
    await expect(page.getByTestId('tour-resume')).toHaveCount(0);
  });

  test('the tour walks the whole first-contact flow on real firmware', async ({ page }) => {
    // The long one: it boots three firmware processes and waits on real key
    // exchange, real flooding and real receipt slots. It runs in about 90
    // seconds on an unloaded box, so this ceiling is a wide starvation margin,
    // not a target.
    test.setTimeout(25 * 60_000);

    const cap: WsCapture = attachWsCapture(page);
    await page.goto('/?tour=1');
    await loadScenarioNoPlay(page, 'emu-playground');

    // --- step 1: what you are looking at -------------------------------
    const fleet = await waitFor<Fleet>(
      () => {
        const alpha = cap.nodeAt(SLOT.alpha, 0);
        const bravo = cap.nodeAt(SLOT.bravo, 0);
        const charlie = cap.nodeAt(SLOT.charlie, 0);
        return alpha && bravo && charlie ? { alpha, bravo, charlie } : undefined;
      },
      { timeoutMs: ATTACH_MS, label: 'the three playground nodes to attach in their slots' },
    );

    // A node attaches to the broker early in boot but can only be driven once
    // it has registered its control-path handlers, which it announces itself.
    // Wait for that on the wire before touching anything: a provision sent
    // before then is dropped by the node with no reply, and the tour's own
    // gate on the same fact is what this is cross-checking.
    await waitFor(
      () =>
        [fleet.alpha, fleet.bravo, fleet.charlie].every((n) =>
          consoleHas(cap, n, 'emu-link control path ready'),
        ) || undefined,
      { timeoutMs: ATTACH_MS, label: 'all three nodes to open their control path' },
    );

    // The tour notices the fleet is up and moves itself on: that is what
    // "steps advance on completion where detectable" means.
    await expectStep(page, 'provision', ATTACH_MS);

    // A page reload must not cost the visitor the fleet. Joins, console lines
    // and framebuffers are one-shot broadcasts, so the broker catches a newly
    // connected client up on the world that already exists (gosim
    // Sim.SnapshotEvents); without that, a reload leaves an empty map, blank
    // pagers and a tour that cannot tell whether the fleet is drivable.
    await page.reload();
    await page.getByTestId('view-tab-devices').click();
    for (const id of [fleet.alpha, fleet.bravo, fleet.charlie]) {
      await expect(page.getByTestId(`device-card-${id}`)).toBeVisible({ timeout: 30_000 });
      const grid = await readCanvasGrid(page, id);
      expect(
        grid.some((row) => row.some(Boolean)),
        `${id} repainted from the broker's snapshot rather than staying blank`,
      ).toBe(true);
    }
    // The console tail comes back too, which is what lets the tour know the
    // fleet is still drivable: its action buttons are gated on each node's
    // control-path line, printed once at boot.
    await expect(page.getByTestId('tour-node-alpha')).toHaveText(/inert/, { timeout: 30_000 });
    await expect(page.getByTestId('tour-action-provision-pair')).toBeEnabled();
    // The tour resolves each role by the scenario slot the node landed in.
    // Assert that mapping against the ids the broker reported on the wire: a
    // silent mismatch would send every action to the wrong pager, and the
    // symptoms downstream would look like a mesh failure rather than a naming
    // one.
    for (const role of ['alpha', 'bravo', 'charlie'] as const) {
      await expect(page.getByTestId(`tour-node-${role}`)).toHaveAttribute(
        'data-node-id',
        fleet[role],
        { timeout: 30_000 },
      );
    }

    await page.getByTestId('view-tab-mesh').click();

    // The fleet view with the tour visible, for docs/playground.md.
    await expect(page.getByTestId('mesh-canvas')).toBeVisible();
    await shoot(page, 'emulator-playground-tour.png');

    // --- step 2: the fail-closed state ---------------------------------
    // Every node booted inert. This is the teaching moment, and it is a
    // firmware fact, not a UI caption.
    await waitFor(
      () =>
        [fleet.alpha, fleet.bravo, fleet.charlie].every((n) =>
          consoleHas(cap, n, 'unprovisioned: no beacon key'),
        ) || undefined,
      { timeoutMs: ATTACH_MS, label: 'all three nodes to report the fail-closed boot state' },
    );

    await page.getByTestId('tour-switch-view').click();
    await expect(page.getByTestId(`device-card-${fleet.charlie}`)).toBeVisible();

    // Key two of the three and leave the third inert on purpose.
    await page.getByTestId('tour-action-provision-pair').click();
    await waitFor(
      () => {
        const on = provisionedNodes(cap);
        return on.includes(fleet.alpha) && on.includes(fleet.bravo) ? on : undefined;
      },
      { timeoutMs: PROVISION_MS, label: 'ALPHA and BRAVO to take the network key' },
    );
    expect(
      provisionedNodes(cap),
      'CHARLIE must still be inert: a fleet-wide provision would erase the teaching moment',
    ).not.toContain(fleet.charlie);
    await expect(page.getByTestId('tour-node-charlie')).toHaveText(/inert/);

    await page.getByTestId('tour-action-provision-rest').click();
    await expectStep(page, 'channel', PROVISION_MS);
    await expect(page.getByTestId('tour-node-charlie')).toHaveText(/provisioned/);

    // --- step 3: a channel message, relayed ----------------------------
    await page.getByTestId('tour-switch-view').click();
    await clickUntil(
      page,
      'tour-action-send-channel',
      () => consoleHas(cap, fleet.charlie, `>>> ${CHANNEL_TEXT}`),
      {
        timeoutMs: DELIVERY_MS,
        everyMs: 20_000,
        label: 'CHARLIE to print a broadcast it can only have received through BRAVO',
      },
    );
    // The control path reports what the send path did with each request, so a
    // send that was refused (an exhausted airtime lane, a busy channel) can
    // never be mistaken for one that was transmitted and lost.
    // CHARLIE sits 200 units from ALPHA with a 150-unit range, so it cannot
    // hear ALPHA at all: the copy it decoded was BRAVO's rebroadcast, and the
    // wait above is therefore already an assertion that the relay happened.
    // The exact route is nailed down at the end of this test, where the
    // delivery receipt comes home carrying its two-hop relay path.
    await expectStep(page, 'dm', DELIVERY_MS);

    // --- step 4: a DM and the 7-digit safety number ---------------------
    await page.getByTestId('tour-switch-view').click();

    // A safety number needs a pinned identity, and a pin only comes from an
    // identity attestation, which a keyed node sends on the firmware's own
    // cadence (once it has a key, then every fifteen minutes). Ask ALPHA to
    // announce now rather than waiting on that cadence.
    await page.getByTestId('tour-action-announce-identity').click();

    await clickUntil(
      page,
      'tour-action-send-dm',
      () => consoleHas(cap, fleet.bravo, `>>> ${DM_TEXT}`),
      {
        timeoutMs: SESSION_MS,
        everyMs: 45_000,
        label: 'the ALPHA-BRAVO key exchange to complete and the DM to land',
      },
    );

    // A peer list can only be opened once there is a peer in it: SELECT on the
    // Nodes screen is a no-op while the neighbour table is empty
    // (components/ui/ui_manager.c gates nodes_selecting on node_total > 0), so
    // pressing into it before BRAVO has heard ALPHA leaves the walk below
    // stranded on the summary screen. Neighbours come from beacons, on this
    // scenario's 15 s cadence and only once the hearing node is keyed, which a
    // DM landing does not imply: a DM reaches BRAVO through the session
    // handshake, not through the neighbour table. Wait for the neighbour line
    // BRAVO prints itself (mesh_beacon.c) before touching its buttons.
    await waitFor(
      () => consoleHas(cap, fleet.bravo, `Neighbor ${fleet.alpha} RSSI`) || undefined,
      { timeoutMs: DELIVERY_MS, label: "BRAVO to hear ALPHA's beacon and hold it as a neighbour" },
    );

    // Now do the verification on the device itself, the way a person would:
    // walk BRAVO to its Nodes screen and open the peer.
    // Each hop is gated on that screen's own unambiguous footer. "[hold]
    // verify" is deliberately NOT used as a target: the plain neighbour list
    // ends in "[hold] verify contacts", so it would match a screen two presses
    // short of the detail view.
    await pressUntil(page, fleet.bravo, 'down', 'Nodes (', 8);
    await pressUntil(page, fleet.bravo, 'select', '[short]next', 4);
    // BRAVO has two neighbours and the cursor starts on the first one, which
    // is whichever the neighbour table happens to list first. Walk it to
    // ALPHA, the peer this step is about.
    await pressUntil(page, fleet.bravo, 'down', '>ALPHA', 6);
    await pressUntil(page, fleet.bravo, 'select', '[2x] back', 4);

    // The safety number is on screen as 7 digits in the large font, grouped
    // 3 + 4 (sas_format_grouped). Read the digits off the panel rather than
    // trusting the label, so this asserts the real derived number.
    //
    // A safety number needs a pinned identity for the peer, which arrives
    // with ALPHA's identity attestation. The detail screen repaints only when
    // something marks it dirty, so a pin landing after the screen was drawn
    // would leave "No secure session yet" on the panel indefinitely; a DOWN
    // press in the detail view repaints it (it only disarms the confirmation,
    // and there is nothing armed yet).
    //
    // The wait is on the number being ON THE PANEL, not on the pinning log
    // line: a pin is announced once and a re-attestation from an already
    // pinned peer is a silent refresh, so a transition line is the wrong
    // thing to wait for. Re-announcing periodically is what a person does
    // when the peer screen still says it has no session, and an attestation
    // is a broadcast that can be lost like any other.
    let poll = 0;
    const sas = await waitFor(
      async () => {
        const found = readSafetyNumber(await readCanvasGrid(page, fleet.bravo));
        if (found) return found;
        if (poll > 0 && poll % 8 === 0) {
          await page.getByTestId('tour-action-announce-identity').click();
        }
        poll += 1;
        await clickButton(page, fleet.bravo, 'down');
        return undefined;
      },
      { timeoutMs: 180_000, intervalMs: 3_000, label: 'the 7-digit safety number to be painted' },
    );
    expect(sas, 'safety number as painted, grouped 3 + 4').toMatch(/^\d{3} \d{4}$/);

    // Two presses: the first arms the confirmation, the second commits it.
    await pressUntil(page, fleet.bravo, 'select', '[hold again] confirm', 3);
    await clickButton(page, fleet.bravo, 'select');

    await waitFor(
      () => consoleHas(cap, fleet.bravo, 'marked VERIFIED (safety number confirmed)') || undefined,
      { timeoutMs: 60_000, label: 'BRAVO to record the confirmed safety number' },
    );
    await expectStep(page, 'receipt', 60_000);

    // --- step 5: the delivery receipt and its relay path -----------------
    await clickUntil(
      page,
      'tour-action-send-receipt',
      () => receiptsAt(cap, fleet.alpha).some((r) => r.path.length >= 2),
      {
        timeoutMs: RECEIPT_MS,
        everyMs: 25_000,
        label: 'a delivery receipt to reach ALPHA having crossed a relay',
      },
    );
    const relayed = receiptsAt(cap, fleet.alpha).find((r) => r.path.length >= 2);
    if (!relayed) throw new Error('no relayed receipt after clickUntil reported one');
    // The path is in travel order, receiver first, so the answering node is
    // its own first hop and the last entry is the relay that handed it to
    // ALPHA. A single-hop receipt from the immediate neighbour proves
    // delivery but not a route, which is why the wait above required two.
    expect(relayed.path[0], 'the receipt names its own sender as the first hop').toBe(relayed.from);
    expect(relayed.path.length, 'relay hops carried by the receipt').toBeGreaterThanOrEqual(2);

    await expect(page.getByTestId('tour-detail')).toContainText('receipt from');
    await expect(page.getByTestId('tour-finished')).toBeVisible();

    // The milestone capture for docs/playground.md.
    await shoot(page, 'emulator-playground-receipt.png');
  });
});
