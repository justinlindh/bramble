// fleet.ts
//
// What the guided tour needs to know about the playground fleet
// (simulator/scenarios/emu-playground.json): which attached firmware node
// plays which part, and the fixed text the tour sends so a milestone can be
// recognised on the wire without guessing.
//
// Roles are resolved by POSITION, not by name. A firmware node's emu-link
// hello id is its own 8-hex firmware address (main.c derives it from the
// identity), which is not knowable ahead of time and carries no hint of which
// scenario slot the node landed in. What is knowable is where the scenario put
// it: the playground is a line at x = 0, 100 and 200, and that line is the
// whole point of the scenario, since it is what makes ALPHA-to-CHARLIE traffic
// have to cross BRAVO. Matching on the slot the broker placed a node in is
// therefore both available and honest.

export type FleetRole = 'alpha' | 'bravo' | 'charlie';

export const FLEET_ROLES: FleetRole[] = ['alpha', 'bravo', 'charlie'];

// Human-facing names, matching each node's EMU_NODE_NAME in the scenario.
export const ROLE_NAMES: Record<FleetRole, string> = {
  alpha: 'ALPHA',
  bravo: 'BRAVO',
  charlie: 'CHARLIE',
};

// Each role's x position in emu-playground.json. Kept here so a change to the
// scenario's geometry shows up as a failing tour test rather than as a tour
// that silently points at the wrong pager.
export const ROLE_X: Record<FleetRole, number> = { alpha: 0, bravo: 100, charlie: 200 };

// How far off its declared slot a node may be and still be recognised. The
// playground never moves its nodes, so this only absorbs float noise; it is
// far below the 100-unit spacing, so two roles can never both claim a node.
const SLOT_TOLERANCE = 25;

export type Fleet = Record<FleetRole, string | null>;

export const EMPTY_FLEET: Fleet = { alpha: null, bravo: null, charlie: null };

// The shape resolveFleet needs from a simulation node: enough to tell a
// firmware pager from a purely simulated node and to find its slot.
export interface FleetCandidate {
  id: string;
  x: number;
  kind?: string;
}

// resolveFleet maps each role onto the attached firmware node standing in its
// scenario slot. Roles with nothing in their slot stay null, which is the
// state the tour renders while the fleet is still booting.
export function resolveFleet(candidates: Iterable<FleetCandidate>): Fleet {
  const firmware = [...candidates].filter((n) => n.kind === 'firmware');
  const out: Fleet = { ...EMPTY_FLEET };
  for (const role of FLEET_ROLES) {
    const match = firmware.find((n) => Math.abs(n.x - ROLE_X[role]) <= SLOT_TOLERANCE);
    out[role] = match ? match.id : null;
  }
  return out;
}

// The demo network key the tour provisions. It is a fixed, published constant
// on purpose: the playground is a teaching fleet on a virtual ether, so this
// key protects nothing and pretending otherwise would be the wrong lesson. A
// real fleet's key is generated per deployment and never lives in a source
// file. Same value the other emu-* scenarios hand out through EMU_NETWORK_KEY,
// which keeps one demo key across the emulator rather than two.
export const PLAYGROUND_NETWORK_KEY =
  '0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20';

// Message texts. Each is distinct so a milestone scan can tell which step's
// traffic it is looking at, and each is short enough to fit one unfragmented
// frame (fragmentation would split the send across several packet ids and make
// the receipt correlation harder to read on the console).
export const CHANNEL_TEXT = 'HELLO PLAYGROUND';
export const DM_TEXT = 'DM FOR BRAVO';
export const RECEIPT_TEXT = 'TRACE THIS ROUTE';
