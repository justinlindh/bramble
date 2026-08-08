// milestones.ts
//
// Turns the firmware consoles the broker streams into the handful of facts the
// guided tour advances on. Everything here reads REAL firmware output: each
// marker below is a log line the shipped firmware prints on the path the tour
// step is teaching, so a step can only complete because the thing actually
// happened on the virtual ether.
//
// The scan is a pure function over the console buffers, and the caller latches
// its result (latchMilestones) because those buffers are a bounded ring: a
// marker that has scrolled off must not un-complete a step the user already
// finished.

import { CHANNEL_TEXT, DM_TEXT, RECEIPT_TEXT } from './fleet';

// Firmware markers, each quoted from the source that prints it.
//
//   INERT     mesh_beacon.c mesh_rederive_beacon_key, the fail-closed state a
//             node boots into with no network key.
//   CONTROL   emulator/node/emu_control.c, the emu-link control path the tour
//             drives the fleet through.
//   PROVISION emu_control.c, after network_key_set_from_hex accepted the key.
//   MESSAGE   mesh_task.c, the received plaintext of a channel message or DM.
//   PINNED    mesh_beacon.c, when an identity attestation was accepted and the
//             peer's keys were pinned. A safety number is derived from a pin,
//             so this is the precondition for the verification flow.
//   VERIFIED  main.c, after mesh_set_peer_verified accepted a safety-number
//             confirmation made on the device.
//   RECEIPT   mesh_reliability.c handle_delivery_receipt, on the ORIGINATOR,
//             carrying the relay path the receipt travelled.
const MARK_INERT = 'unprovisioned: no beacon key';
const MARK_CONTROL_READY = 'emu-link control path ready';
const MARK_PROVISIONED = 'network key provisioned over emu-link';
const MARK_VERIFIED = 'marked VERIFIED (safety number confirmed)';
const MARK_MESSAGE_PREFIX = '>>> ';

const PINNED_RE = /Identity pinned: ([0-9A-F]{8})/;

const RECEIPT_RE =
  /Delivery receipt from ([0-9A-F]{8}) for broadcast ([0-9A-F]{8}) \((\d+) relay hop\(s\)(?: via ([0-9A-F>]+))?\)/;

// One delivery receipt as the originating node logged it.
export interface ReceiptRecord {
  // Node id (emu-link hello id) whose console printed the receipt: the
  // ORIGINATOR of the broadcast being confirmed.
  at: string;
  // Address of the node that received the broadcast and answered.
  from: string;
  // Packet id of the broadcast being confirmed.
  broadcast: string;
  // Relay hops the receipt carried, receiver first (travel order).
  path: string[];
}

// One peer identity a node has pinned.
export interface PinRecord {
  // Node id (emu-link hello id) that did the pinning.
  at: string;
  // The pinned peer's address. A firmware node's emu-link hello id is that
  // same address, so this is directly comparable to a node id.
  peer: string;
}

export interface Milestones {
  // Nodes that logged the unprovisioned fail-closed boot state.
  inert: string[];
  // Nodes whose emu-link control path came up (they can be driven).
  controlReady: string[];
  // Nodes that took a network key at runtime.
  provisioned: string[];
  // Nodes that printed the tour's channel-broadcast text.
  channelHeardBy: string[];
  // Nodes that printed the tour's DM text.
  dmHeardBy: string[];
  // Peer identities that have been pinned, and by whom.
  pins: PinRecord[];
  // Nodes that recorded a safety-number confirmation made on their face.
  verified: string[];
  // Nodes that printed the tour's receipt-step broadcast text.
  receiptTextHeardBy: string[];
  // Delivery receipts that came home, newest last.
  receipts: ReceiptRecord[];
}

export const EMPTY_MILESTONES: Milestones = {
  inert: [],
  controlReady: [],
  provisioned: [],
  channelHeardBy: [],
  dmHeardBy: [],
  pins: [],
  verified: [],
  receiptTextHeardBy: [],
  receipts: [],
};

function add(list: string[], value: string): void {
  if (!list.includes(value)) list.push(value);
}

// scanConsoles reads every console buffer once and reports which markers are
// present in it. Pure: same input, same output, no latching.
export function scanConsoles(consoles: Iterable<readonly [string, readonly string[]]>): Milestones {
  const m: Milestones = {
    inert: [],
    controlReady: [],
    provisioned: [],
    channelHeardBy: [],
    dmHeardBy: [],
    pins: [],
    verified: [],
    receiptTextHeardBy: [],
    receipts: [],
  };

  for (const [node, lines] of consoles) {
    for (const line of lines) {
      if (line.includes(MARK_INERT)) add(m.inert, node);
      if (line.includes(MARK_CONTROL_READY)) add(m.controlReady, node);
      if (line.includes(MARK_PROVISIONED)) add(m.provisioned, node);
      if (line.includes(MARK_VERIFIED)) add(m.verified, node);
      if (line.includes(MARK_MESSAGE_PREFIX + CHANNEL_TEXT)) add(m.channelHeardBy, node);
      if (line.includes(MARK_MESSAGE_PREFIX + DM_TEXT)) add(m.dmHeardBy, node);
      if (line.includes(MARK_MESSAGE_PREFIX + RECEIPT_TEXT)) add(m.receiptTextHeardBy, node);

      const pin = PINNED_RE.exec(line);
      if (pin && !m.pins.some((p) => p.at === node && p.peer === pin[1])) {
        m.pins.push({ at: node, peer: pin[1] });
      }

      const rec = RECEIPT_RE.exec(line);
      if (rec) {
        m.receipts.push({
          at: node,
          from: rec[1],
          broadcast: rec[2],
          path: rec[4] ? rec[4].split('>') : [],
        });
      }
    }
  }

  return m;
}

function union(a: readonly string[], b: readonly string[]): string[] {
  const out = [...a];
  for (const v of b) add(out, v);
  return out;
}

// latchMilestones folds a fresh scan into what has already been seen. Console
// buffers are bounded rings, so a marker can scroll out of view; a completed
// step must stay completed, which is exactly what this union guarantees.
// Receipts are keyed by (originator, confirming node, broadcast id) so the
// same receipt seen in two consecutive scans is not counted twice.
export function latchMilestones(prev: Milestones, next: Milestones): Milestones {
  const pins = [...prev.pins];
  for (const p of next.pins) {
    if (!pins.some((q) => q.at === p.at && q.peer === p.peer)) pins.push(p);
  }
  const receipts = [...prev.receipts];
  for (const r of next.receipts) {
    const dup = receipts.some(
      (p) => p.at === r.at && p.from === r.from && p.broadcast === r.broadcast,
    );
    if (!dup) receipts.push(r);
  }
  return {
    inert: union(prev.inert, next.inert),
    controlReady: union(prev.controlReady, next.controlReady),
    provisioned: union(prev.provisioned, next.provisioned),
    channelHeardBy: union(prev.channelHeardBy, next.channelHeardBy),
    dmHeardBy: union(prev.dmHeardBy, next.dmHeardBy),
    pins,
    verified: union(prev.verified, next.verified),
    receiptTextHeardBy: union(prev.receiptTextHeardBy, next.receiptTextHeardBy),
    receipts,
  };
}

// hasPinned reports whether `at` has pinned `peer`'s identity, which is what
// makes a safety number derivable for that peer.
export function hasPinned(m: Milestones, at: string | null, peer: string | null): boolean {
  if (!at || !peer) return false;
  return m.pins.some((p) => p.at === at && p.peer === peer);
}

// multiHopReceipt returns the first receipt whose relay path crossed more than
// one node, which is the one the tour's last step is about: a single-hop
// receipt from an immediate neighbour proves delivery but not a route.
export function multiHopReceipt(m: Milestones): ReceiptRecord | null {
  return m.receipts.find((r) => r.path.length >= 2) ?? null;
}
