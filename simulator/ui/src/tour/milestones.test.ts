import { describe, expect, it } from 'vitest';
import {
  EMPTY_MILESTONES,
  hasPinned,
  latchMilestones,
  multiHopReceipt,
  scanConsoles,
} from './milestones';
import { CHANNEL_TEXT, DM_TEXT, RECEIPT_TEXT, ROLE_X, resolveFleet } from './fleet';
import { TOUR_STEPS } from './steps';

// Console lines quoted verbatim from the firmware that prints them, so a
// wording change in the firmware breaks these tests rather than silently
// breaking the tour's ability to advance.
const INERT = 'W (812) mesh: unprovisioned: no beacon key (node inert until provisioned)';
const CONTROL = 'I (830) emu_control: emu-link control path ready (prov, send)';
const PROVISIONED =
  'I (9421) emu_control: network key provisioned over emu-link (fingerprint 2FA10C3B)';
const VERIFIED =
  'I (52110) main: Peer 1A2B3C4D marked VERIFIED (safety number confirmed)';
const RECEIPT_2HOP =
  'I (61233) mesh: Delivery receipt from 3C4D5E6F for broadcast 00000517 (2 relay hop(s) via 3C4D5E6F>2B3C4D5E)';
const PINNED = 'I (41007) mesh: Identity pinned: 1A2B3C4D (1 pinned)';
const RECEIPT_1HOP =
  'I (60110) mesh: Delivery receipt from 2B3C4D5E for broadcast 00000517 (1 relay hop(s) via 2B3C4D5E)';

// The firmware ids are the nodes' own 8-hex addresses in a real run; the
// readable stand-ins here keep the assertions legible. What matters is the
// slot each one occupies, which is how the tour resolves roles.
const FIRMWARE = [
  { id: 'alpha-0', x: 0, kind: 'firmware' },
  { id: 'bravo-0', x: 100, kind: 'firmware' },
  { id: 'charlie-0', x: 200, kind: 'firmware' },
];

describe('scanConsoles', () => {
  it('finds the fail-closed boot marker per node', () => {
    const m = scanConsoles([
      ['alpha-0', [INERT, CONTROL]],
      ['bravo-0', [CONTROL]],
    ]);
    expect(m.inert).toEqual(['alpha-0']);
    expect(m.controlReady).toEqual(['alpha-0', 'bravo-0']);
  });

  it('records a runtime provision only for the node that logged it', () => {
    const m = scanConsoles([
      ['alpha-0', [INERT, PROVISIONED]],
      ['charlie-0', [INERT]],
    ]);
    expect(m.provisioned).toEqual(['alpha-0']);
  });

  it('separates the three tour message texts', () => {
    const m = scanConsoles([
      ['charlie-0', [`I (1) mesh: >>> ${CHANNEL_TEXT}`, `I (2) mesh: >>> ${RECEIPT_TEXT}`]],
      ['bravo-0', [`I (3) mesh: >>> ${DM_TEXT}`]],
    ]);
    expect(m.channelHeardBy).toEqual(['charlie-0']);
    expect(m.receiptTextHeardBy).toEqual(['charlie-0']);
    expect(m.dmHeardBy).toEqual(['bravo-0']);
  });

  it('parses a delivery receipt into its relay path', () => {
    const m = scanConsoles([['alpha-0', [RECEIPT_2HOP]]]);
    expect(m.receipts).toEqual([
      { at: 'alpha-0', from: '3C4D5E6F', broadcast: '00000517', path: ['3C4D5E6F', '2B3C4D5E'] },
    ]);
  });

  it('records which peer identity a node pinned', () => {
    const m = scanConsoles([['bravo-0', [PINNED]]]);
    expect(m.pins).toEqual([{ at: 'bravo-0', peer: '1A2B3C4D' }]);
    expect(hasPinned(m, 'bravo-0', '1A2B3C4D')).toBe(true);
    expect(hasPinned(m, 'charlie-0', '1A2B3C4D')).toBe(false);
  });

  it('records the safety-number confirmation the device made', () => {
    const m = scanConsoles([['bravo-0', [VERIFIED]]]);
    expect(m.verified).toEqual(['bravo-0']);
  });
});

describe('latchMilestones', () => {
  it('keeps a marker that has scrolled out of the console ring', () => {
    const first = scanConsoles([['alpha-0', [PROVISIONED]]]);
    const latched = latchMilestones(EMPTY_MILESTONES, first);
    // The node's console ring has rolled over and no longer holds the line.
    const later = latchMilestones(latched, scanConsoles([['alpha-0', ['I (99) mesh: beacon sent']]]));
    expect(later.provisioned).toEqual(['alpha-0']);
  });

  it('does not count the same receipt twice across scans', () => {
    const scan = scanConsoles([['alpha-0', [RECEIPT_2HOP]]]);
    const once = latchMilestones(EMPTY_MILESTONES, scan);
    const twice = latchMilestones(once, scan);
    expect(twice.receipts).toHaveLength(1);
  });
});

describe('multiHopReceipt', () => {
  it('ignores a neighbour receipt and returns the relayed one', () => {
    const m = latchMilestones(
      EMPTY_MILESTONES,
      scanConsoles([['alpha-0', [RECEIPT_1HOP, RECEIPT_2HOP]]]),
    );
    expect(multiHopReceipt(m)?.path).toEqual(['3C4D5E6F', '2B3C4D5E']);
  });

  it('is null when only immediate neighbours have answered', () => {
    const m = latchMilestones(EMPTY_MILESTONES, scanConsoles([['alpha-0', [RECEIPT_1HOP]]]));
    expect(multiHopReceipt(m)).toBeNull();
  });
});

describe('resolveFleet', () => {
  it('maps scenario slots onto the firmware ids that landed in them', () => {
    expect(resolveFleet(FIRMWARE)).toEqual({
      alpha: 'alpha-0',
      bravo: 'bravo-0',
      charlie: 'charlie-0',
    });
  });

  it('leaves a role null while its node is still booting', () => {
    expect(resolveFleet([FIRMWARE[0]])).toEqual({ alpha: 'alpha-0', bravo: null, charlie: null });
  });

  it('ignores purely simulated nodes sitting in the same slots', () => {
    expect(
      resolveFleet([
        { id: 'sim-a', x: ROLE_X.alpha, kind: 'sim' },
        { id: 'sim-b', x: ROLE_X.bravo },
      ]),
    ).toEqual({ alpha: null, bravo: null, charlie: null });
  });
});

describe('tour steps', () => {
  const fleet = resolveFleet(FIRMWARE);
  const byId = (id: string) => {
    const s = TOUR_STEPS.find((step) => step.id === id);
    if (!s) throw new Error(`no step ${id}`);
    return s;
  };

  it('walks the five required subjects in order', () => {
    expect(TOUR_STEPS.map((s) => s.id)).toEqual([
      'orientation',
      'provision',
      'channel',
      'dm',
      'receipt',
    ]);
  });

  it('finishes orientation only once every node can actually be driven', () => {
    const allReady = scanConsoles(FIRMWARE.map((n) => [n.id, [CONTROL]] as const));
    // Attached but the control path is not up yet: the provisioning buttons
    // on the next step would be dropped on the floor, so the step holds.
    expect(byId('orientation').done(EMPTY_MILESTONES, fleet)).toBe(false);
    // Ready, but one node has not attached at all.
    expect(byId('orientation').done(allReady, resolveFleet([FIRMWARE[0]]))).toBe(false);
    expect(byId('orientation').done(allReady, fleet)).toBe(true);
  });

  it('holds the provisioning step open while any node is still inert', () => {
    const partial = latchMilestones(
      EMPTY_MILESTONES,
      scanConsoles([
        ['alpha-0', [PROVISIONED]],
        ['bravo-0', [PROVISIONED]],
        ['charlie-0', [INERT]],
      ]),
    );
    expect(byId('provision').done(partial, fleet)).toBe(false);
    const all = latchMilestones(partial, scanConsoles([['charlie-0', [PROVISIONED]]]));
    expect(byId('provision').done(all, fleet)).toBe(true);
  });

  it('finishes the channel step only when the out-of-range node heard it', () => {
    const bravoOnly = scanConsoles([['bravo-0', [`I (1) mesh: >>> ${CHANNEL_TEXT}`]]]);
    expect(byId('channel').done(bravoOnly, fleet)).toBe(false);
    const charlieToo = latchMilestones(
      bravoOnly,
      scanConsoles([['charlie-0', [`I (2) mesh: >>> ${CHANNEL_TEXT}`]]]),
    );
    expect(byId('channel').done(charlieToo, fleet)).toBe(true);
  });

  it('needs both the DM and the safety-number confirmation', () => {
    const dmOnly = scanConsoles([['bravo-0', [`I (1) mesh: >>> ${DM_TEXT}`]]]);
    expect(byId('dm').done(dmOnly, fleet)).toBe(false);
    const verified = latchMilestones(dmOnly, scanConsoles([['bravo-0', [VERIFIED]]]));
    expect(byId('dm').done(verified, fleet)).toBe(true);
  });

  it('says whether the peer identity is pinned, which is what gates the number', () => {
    // Real node ids are the firmware's own 8-hex address, and a pin line names
    // the peer by that same address; this fixture uses those so the detail
    // reads exactly as it does in a live run.
    const hexFleet = resolveFleet([
      { id: '1A2B3C4D', x: 0, kind: 'firmware' },
      { id: '2B3C4D5E', x: 100, kind: 'firmware' },
      { id: '3C4D5E6F', x: 200, kind: 'firmware' },
    ]);
    const dmOnly = scanConsoles([['2B3C4D5E', [`I (1) mesh: >>> ${DM_TEXT}`]]]);
    expect(byId('dm').detail(dmOnly, hexFleet)).toContain('no identity pin for ALPHA');

    const pinned = latchMilestones(dmOnly, scanConsoles([['2B3C4D5E', [PINNED]]]));
    expect(byId('dm').detail(pinned, hexFleet)).toContain("pinned ALPHA's identity");
  });

  it('needs a receipt that crossed a relay, not just a neighbour', () => {
    const oneHop = scanConsoles([['alpha-0', [RECEIPT_1HOP]]]);
    expect(byId('receipt').done(oneHop, fleet)).toBe(false);
    const twoHop = latchMilestones(oneHop, scanConsoles([['alpha-0', [RECEIPT_2HOP]]]));
    expect(byId('receipt').done(twoHop, fleet)).toBe(true);
    expect(byId('receipt').detail(twoHop, fleet)).toContain('3C4D5E6F > 2B3C4D5E');
  });

  it('points every step at a real UI element once the fleet is up', () => {
    for (const step of TOUR_STEPS) {
      expect(step.target(fleet), `step ${step.id} target`).toBeTruthy();
    }
  });
});
