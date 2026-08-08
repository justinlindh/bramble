// Attested roll-call: start one, and read the initiator's ledger.
//
// The primitive is documented in docs/rollcall.md. What matters at this layer
// is the honesty switch: on an un-anchored node there is no authoritative
// expected set, so the firmware reports expected 0 and an empty missing list
// BY CONSTRUCTION, and the UI must present the ledger as observed responders
// rather than as a complete fleet answer.
import { requireClient } from './client';
import { tryParseAddr } from '../../lib/addr';
import type { RollCallLedger, RollCallResponder, RollCallStart } from '../../types/bramble';
import type { RpcSchemas, WirePartial } from '../../types/rpc';

type StartWire = WirePartial<RpcSchemas['StartRollCallResponse']>;
type LedgerWire = WirePartial<RpcSchemas['RollCallLedger']>;

// tryParseAddr, not parseAddr: a ledger address the firmware could not have
// sent is dropped rather than truncated to whatever leading hex digits it
// happened to start with, which would put a node that does not exist in an
// operator's missing list.
function toAddrs(hexes: (string | undefined)[] | undefined): number[] {
  if (!hexes) return [];
  const out: number[] = [];
  for (const h of hexes) {
    if (typeof h !== 'string') continue;
    const addr = tryParseAddr(h);
    if (addr !== null) out.push(addr);
  }
  return out;
}

function normalizeResponder(raw: NonNullable<LedgerWire['responders']>[number]): RollCallResponder | null {
  const addr = typeof raw.address === 'string' ? tryParseAddr(raw.address) : null;
  if (addr === null) return null;
  const path = toAddrs(raw.path as (string | undefined)[] | undefined);
  return {
    addr,
    responded: raw.responded ?? false,
    atMs: raw.at_ms,
    round: raw.round,
    relayPath: path.length > 0 ? path : undefined,
  };
}

export function normalizeRollCallLedger(raw: LedgerWire): RollCallLedger {
  const responders: RollCallResponder[] = [];
  for (const r of raw.responders ?? []) {
    const row = normalizeResponder(r);
    if (row) responders.push(row);
  }
  return {
    active: raw.active ?? false,
    open: raw.open ?? false,
    rollcallId: raw.rollcall_id,
    text: raw.text,
    roundsSent: raw.rounds_sent ?? 0,
    roundsTotal: raw.rounds_total ?? 0,
    windowMs: raw.window_ms ?? 0,
    elapsedMs: raw.elapsed_ms ?? 0,
    minIntervalMs: raw.min_interval_ms ?? 0,
    maxTextBytes: raw.max_text_bytes ?? 0,
    anchored: raw.anchored ?? false,
    expected: raw.expected ?? 0,
    responded: raw.responded ?? 0,
    unattested: raw.unattested ?? 0,
    overflow: raw.overflow ?? 0,
    late: raw.late ?? 0,
    pendingDropped: raw.pending_dropped ?? 0,
    missing: toAddrs(raw.missing as (string | undefined)[] | undefined),
    missingCount: raw.missing_count ?? 0,
    responders,
  };
}

export async function loadRollCall(): Promise<RollCallLedger> {
  const client = requireClient();
  const raw = await client.rpc<LedgerWire>('bramble.getRollCall');
  return normalizeRollCallLedger(raw ?? {});
}

export async function startRollCall(text: string): Promise<RollCallStart> {
  const client = requireClient();
  const trimmed = text.trim();
  const raw = await client.rpc<StartWire>(
    'bramble.startRollCall',
    trimmed ? { text: trimmed } : {},
  );
  return {
    ok: raw?.ok ?? false,
    rollcallId: raw?.rollcall_id,
    reason: raw?.reason,
    retryAfterMs: raw?.retry_after_ms,
    expected: raw?.expected ?? 0,
    anchored: raw?.anchored ?? false,
  };
}
