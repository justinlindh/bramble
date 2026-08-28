// Probe / Network Reach: send a mesh reachability probe and fold the firmware's
// ack and completion pushes into the probeResult store slice. This is a network
// diagnostic, sibling to the attested roll-call in rollcall.ts, kept out of the
// messaging module so a reader finds it where the feature lives.
import { requireClient } from './client';
import { useStore } from '../index';
import { parseAddr } from '../../lib/addr';
import type { ProbeResponse } from '../../types/bramble';

export function upsertProbeResponse(responses: ProbeResponse[], next: ProbeResponse): ProbeResponse[] {
  const idx = responses.findIndex(r => r.responderAddr === next.responderAddr);
  if (idx < 0) return [...responses, next];

  const prev = responses[idx];
  const merged: ProbeResponse = {
    ...prev,
    ...next,
    // keep best quality samples
    rssi: Math.max(prev.rssi ?? -999, next.rssi ?? -999),
    snr: Math.max(prev.snr ?? -999, next.snr ?? -999),
    // keep latest receive timestamp/latency if present in next
    receivedAt: next.receivedAt ?? prev.receivedAt,
    latencyMs: next.latencyMs ?? prev.latencyMs,
    seenRounds: Math.max(prev.seenRounds ?? 1, next.seenRounds ?? 1),
    confidence: Math.max(prev.confidence ?? 0, next.confidence ?? 0),
  };

  const out = [...responses];
  out[idx] = merged;
  return out;
}

export async function sendProbe(): Promise<void> {
  const client = requireClient();
  const store = useStore.getState();

  const raw = await client.rpc<Record<string, unknown>>('bramble.sendProbe');
  /* Firmware returns probe_id (snake_case hex string), no ackWindow */
  const probeIdStr = (raw.probeId ?? raw.probe_id) as string | undefined;
  const probeId = probeIdStr ? parseInt(probeIdStr, 16) : Math.floor(Math.random() * 0xFFFFFFFF);
  const ackWindow = (raw.ackWindow ?? raw.ack_window ?? 30) as number;
  const sentAt = Date.now();

  store.setProbeResult({
    probeId,
    sentAt,
    ackWindow,
    responses: [],
    complete: false,
  });

  /* Firmware emits bramble.onProbeComplete once its collection window
   * elapses (mesh_task.c), and handleProbeComplete finalizes on it. This
   * auto-finalize at ack-window expiry is the fallback for when that
   * notification never arrives (dropped push, session reconnect), so the UI
   * cannot get stuck in Collecting. */
  setTimeout(() => {
    const s = useStore.getState();
    const cur = s.probeResult;
    if (!cur) return;
    if (cur.probeId !== probeId) return;      /* newer probe started */
    if (cur.complete) return;
    s.setProbeResult({ ...cur, complete: true });
  }, Math.max(1, ackWindow) * 1000 + 150);
}

// Probe push payloads (bramble.onProbeResult / bramble.onProbeComplete). The
// contract does declare these as OnProbeResultPayload and
// OnProbeCompletePayload, but those schemas are strict and snake_case only,
// so the tolerant shape is declared here: every field optional, contract-style
// snake_case plus camelCase fallbacks in case a bridge normalizes keys.
interface ProbeResponderWire {
  address?: string;
  responderAddr?: number;
  seen_rounds?: number;
  seenRounds?: number;
  hops?: number;
  hopCount?: number;
  rssi?: number;
  snr?: number;
  pathLen?: number;
  latency_ms?: number;
  latencyMs?: number;
}

interface ProbeAckWire extends ProbeResponderWire {
  rounds_total?: number;
  roundsTotal?: number;
  probeId?: string | number;
  probe_id?: string | number;
}

interface ProbeCompleteWire {
  probeId?: string | number;
  probe_id?: string | number;
  rounds_total?: number;
  roundsTotal?: number;
  responders?: ProbeResponderWire[];
}

// Map a tolerant responder payload to a ProbeResponse. Shared by the per-ack
// (handleProbeAck) and batch (handleProbeComplete) paths, which otherwise drift
// apart on the field fallbacks and the seenRounds/confidence math.
function normalizeProbeResponder(r: ProbeResponderWire, roundsTotal: number): ProbeResponse {
  const seenRounds = Math.max(1, Math.min(roundsTotal, Number(r.seen_rounds ?? r.seenRounds ?? 1)));
  return {
    responderAddr: parseAddr(r.address ?? r.responderAddr),
    hopCount: r.hops ?? r.hopCount ?? 0,
    rssi: r.rssi ?? 0,
    snr: r.snr ?? 0,
    pathLen: r.hops ?? r.pathLen ?? 0,
    latencyMs: r.latency_ms ?? r.latencyMs ?? 0,
    seenRounds,
    confidence: seenRounds / roundsTotal,
  };
}

// Probe ids arrive as either a hex string (firmware snake_case probe_id) or a
// number; parse the string form, pass a number through, and yield undefined
// when neither field is present. Shared by the per-ack and batch handlers so
// the two paths cannot drift on the fallback order.
function parseProbeId(raw: { probeId?: string | number; probe_id?: string | number }): number | undefined {
  if (typeof raw.probeId === 'string') return parseInt(raw.probeId, 16);
  if (typeof raw.probe_id === 'string') return parseInt(raw.probe_id, 16);
  return raw.probeId ?? raw.probe_id ?? undefined;
}

export function handleProbeAck(params: unknown): void {
  const raw = params as ProbeAckWire;

  const roundsTotal = Math.max(1, Number(raw.rounds_total ?? raw.roundsTotal ?? (raw.seen_rounds ? 3 : 1)));
  const ack = normalizeProbeResponder(raw, roundsTotal);
  const probeId = parseProbeId(raw);

  const store = useStore.getState();
  const prev = store.probeResult;
  if (!prev || prev.complete) return;
  if (probeId !== undefined && probeId !== prev.probeId) return;

  const selfAddr = store.config?.identity?.address;
  if (selfAddr !== undefined && ack.responderAddr === selfAddr) return;

  store.setProbeResult({
    ...prev,
    responses: upsertProbeResponse(prev.responses, { ...ack, receivedAt: Date.now() }),
  });
}

export function handleProbeComplete(params: unknown): void {
  const p = params as ProbeCompleteWire;
  const probeId = parseProbeId(p);

  const store = useStore.getState();
  const prev = store.probeResult;
  if (!prev || prev.probeId !== probeId) return;

  const roundsTotal = Math.max(1, Number(p.rounds_total ?? p.roundsTotal ?? 3));
  const responders = Array.isArray(p.responders) ? p.responders : [];

  let responses = prev.responses;
  for (const r of responders) {
    responses = upsertProbeResponse(responses, normalizeProbeResponder(r, roundsTotal));
  }

  store.setProbeResult({ ...prev, responses, complete: true });
}
