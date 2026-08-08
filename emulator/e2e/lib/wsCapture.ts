// wsCapture.ts
//
// Attaches to the browser page's WebSocket connection to the gosim broker and
// records every frame verbatim, independent of the React app's parsing
// (useSimulation.ts). This gives the test ground truth off the wire: what the
// server actually sent, and what the UI actually sent back, not what the app
// claims it received. Used for:
//   - grabbing the raw base64 "fb" payload of a device_fb event (decoded by
//     fbWire.ts, never by the app's framebuffer.ts) for the fb-vs-canvas
//     comparison.
//   - asserting the outgoing { type:"btn", node, id, edge } frame verbatim
//     when a face button is clicked.
//   - watching node identity (hello id) across a RESET reboot.
//   - full-refresh detection (kind:"full" device_fb events) for the
//     inversion-flash sequence spec.

import type { Page } from '@playwright/test';

export interface FbEvent {
  node: string;
  seq: number;
  kind: 'partial' | 'full';
  fb: string; // base64, raw wire payload
  busyMs: number;
  t: number; // capture-time ms (performance.now() equivalent, Date.now())
}

export interface JoinEvent {
  node: string;
  x: number;
  y: number;
  t: number;
}

export interface ConsoleEvent {
  node: string;
  line: string;
  t: number;
}

export interface SentBtn {
  node: string;
  id: string;
  edge: string;
  t: number;
}

export class WsCapture {
  fbEvents: FbEvent[] = [];
  joinEvents: JoinEvent[] = [];
  consoleEvents: ConsoleEvent[] = [];
  sentBtn: SentBtn[] = [];
  raw: unknown[] = [];

  // Public so attachWsCapture (below) can drive them; not part of the
  // intended test-facing API (use the fbEvents/joinEvents/etc arrays instead).
  onMessage(text: string) {
    let msg: Record<string, unknown>;
    try {
      msg = JSON.parse(text);
    } catch {
      return;
    }

    // A sim_reset means the broker tore the previous world down and is about
    // to build a new one, so everything recorded so far describes a scenario
    // that no longer exists. Dropping it here is what keeps a spec's view
    // scoped to the scenario IT loaded, and it matters because the broker
    // catches a newly connected client up on whatever world already exists
    // (gosim Sim.SnapshotEvents): without this, a spec that opens a page while
    // a previous spec's scenario is still loaded would start with that
    // scenario's nodes and frames in its capture, and node ids resolved by
    // slot position would name the wrong device. The app's own reducer does
    // exactly this on sim_reset; the capture mirrors it.
    if (msg.type === 'sim_reset') {
      this.fbEvents = [];
      this.joinEvents = [];
      this.consoleEvents = [];
      this.sentBtn = [];
      this.raw = [];
    }

    this.raw.push(msg);
    const t = Date.now();
    switch (msg.type) {
      case 'device_fb': {
        const node = msg.node as string | undefined;
        const fb = msg.fb as string | undefined;
        if (node && fb) {
          this.fbEvents.push({
            node,
            seq: this.fbEvents.filter((e) => e.node === node).length + 1,
            kind: (msg.kind as 'partial' | 'full') ?? 'full',
            fb,
            busyMs: (msg.busy_ms as number) ?? 0,
            t,
          });
        }
        break;
      }
      case 'node_joined': {
        const node = msg.node as string | undefined;
        if (node) {
          this.joinEvents.push({ node, x: (msg.x as number) ?? 0, y: (msg.y as number) ?? 0, t });
        }
        break;
      }
      case 'console': {
        const node = msg.node as string | undefined;
        const line = msg.line as string | undefined;
        if (node && typeof line === 'string') {
          this.consoleEvents.push({ node, line, t });
        }
        break;
      }
    }
  }

  onSent(text: string) {
    let msg: Record<string, unknown>;
    try {
      msg = JSON.parse(text);
    } catch {
      return;
    }
    if (msg.type === 'btn') {
      this.sentBtn.push({
        node: msg.node as string,
        id: msg.id as string,
        edge: msg.edge as string,
        t: Date.now(),
      });
    }
  }

  fbFor(node: string): FbEvent[] {
    return this.fbEvents.filter((e) => e.node === node);
  }

  distinctNodes(): string[] {
    return [...new Set(this.joinEvents.map((e) => e.node))];
  }

  nodeAt(x: number, y: number): string | undefined {
    return this.joinEvents.find((e) => e.x === x && e.y === y)?.node;
  }
}

// attachWsCapture must be called BEFORE page.goto so the listener is in place
// before the app opens its WebSocket on mount.
export function attachWsCapture(page: Page): WsCapture {
  const cap = new WsCapture();
  page.on('websocket', (ws) => {
    ws.on('framereceived', (frame) => {
      const text = typeof frame.payload === 'string' ? frame.payload : frame.payload.toString('utf-8');
      cap.onMessage(text);
    });
    ws.on('framesent', (frame) => {
      const text = typeof frame.payload === 'string' ? frame.payload : frame.payload.toString('utf-8');
      cap.onSent(text);
    });
  });
  return cap;
}

// waitFor polls `check` until it returns a truthy value or timeoutMs elapses.
// Used instead of a fixed sleep for anything that depends on real-time
// delivery (message arrival, reboot/reattach) so tests wait exactly as long
// as needed and no longer, per the determinism requirement (no fixed sleeps
// for delivery).
export async function waitFor<T>(
  check: () => T | undefined | null | false | Promise<T | undefined | null | false>,
  opts: { timeoutMs: number; intervalMs?: number; label?: string },
): Promise<T> {
  const interval = opts.intervalMs ?? 250;
  const deadline = Date.now() + opts.timeoutMs;
  for (;;) {
    // await here handles both sync and async `check` callbacks uniformly (a
    // sync return value passes through `await` unchanged); without it, an
    // async check's pending Promise -- always truthy -- would satisfy `if
    // (v)` on the very first poll regardless of what it resolves to.
    const v = await check();
    if (v) return v;
    if (Date.now() >= deadline) {
      throw new Error(`waitFor: timeout after ${opts.timeoutMs}ms${opts.label ? ` waiting for ${opts.label}` : ''}`);
    }
    await new Promise((r) => setTimeout(r, interval));
  }
}
