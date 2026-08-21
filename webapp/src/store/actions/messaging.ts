// Messaging: the message store lifecycle, firmware message merge, send paths,
// delivery-event replay and correlation, broadcast telemetry, probe flows,
// incoming-message handling, and native notifications.
import { session, requireClient, LAST_NODE_ADDR_KEY } from './client';
import { useStore, conversationIdForMessage, formatConversationLabel } from '../index';
import { messageDb } from '../messageDb';
import { deliveryEventStore, type DeliveryEventRecord } from '../deliveryEventStore';
import { formatAddrHex, formatAddr0x } from '../../utils/address';
import { utf8Length, FRAGMENTED_MAX_BYTES } from '../../utils/byteLimit';
import { parseAddr } from '../../lib/addr';
import { isUnknownMethodError } from '../../lib/errors';
import { mergeBroadcastRecipient } from '../../lib/broadcastRecipients';
import { isAndroidShell } from '../../utils/platform';
import { safeGetItem, safeSetItem } from '../../utils/safeLocalStorage';
import type { RelayHop, MessageTier, ProbeResponse, Message } from '../../types/bramble';
import type { RpcSchemas, WirePartial } from '../../types/rpc';
import type { NativeMessageNotification } from '../../types/desktop';

// A message row as the firmware sends it: the contract's Message schema made
// deep-optional (older firmware omits fields) plus the extras the firmware
// includes beyond the schema (msgId, tier, channelIndex). `text` stays
// required: every consumer copies it into a store Message verbatim.
type FirmwareMessageWire = WirePartial<RpcSchemas['Message']> & {
  text: string;
  msgId?: string;
  channelIndex?: number;
  tier?: MessageTier;
};

// ─── Message persistence ─────────────────────────────────────────────────

export async function initMessageStore(nodeAddr?: string): Promise<void> {
  try {
    await Promise.all([
      messageDb.open(nodeAddr),
      deliveryEventStore.open(nodeAddr),
    ]);

    await deliveryEventStore.pruneOldEvents(retentionCutoffTs());

    const cached = await messageDb.getMessages();
    const hydrated = cached.length > 0 ? await hydrateMessagesWithDeliveryEvents(cached) : [];
    hydrateCorrelationMaps(hydrated);
    if (hydrated.length > 0) {
      useStore.getState().loadCachedMessages(hydrated);
    }
  } catch {
    // IndexedDB unavailable (e.g. private browsing), continue without persistence
  }
}

// crypto.randomUUID() requires secure context (HTTPS/localhost).
// Fallback for plain HTTP access over LAN.
function uuid(): string {
  if (typeof crypto !== 'undefined' && typeof crypto.randomUUID === 'function') {
    return crypto.randomUUID();
  }
  return 'xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx'.replace(/[xy]/g, c => {
    const r = (Math.random() * 16) | 0;
    return (c === 'x' ? r : (r & 0x3) | 0x8).toString(16);
  });
}

function isLikelyDuplicate(existing: Message, candidate: Message): boolean {
  if (existing.direction !== candidate.direction) return false;
  if (existing.from !== candidate.from || existing.to !== candidate.to) return false;
  if ((existing.channelIndex ?? -1) !== (candidate.channelIndex ?? -1)) return false;
  if (existing.text !== candidate.text) return false;
  return Math.abs(existing.timestampMs - candidate.timestampMs) < 5000;
}

export interface FirmwareMergeContext {
  /** Messages already known to the store, deduped against. */
  existing: Message[];
  /** Device uptime in seconds, used to convert uptime stamps to wall clock. */
  deviceUptime: number;
  /** This node's own address, used to drop self-addressed rows. */
  myAddr: number;
  /** Wall-clock reference for the conversion, injected so callers stay deterministic. */
  now: number;
}

/**
 * Normalize a `bramble.getMessages` batch into store messages, dropping any
 * that duplicate an existing message or an earlier entry in the same batch.
 *
 * Pure on purpose: the previous inline version deduped against a single
 * `useStore.getState()` snapshot taken before the loop while writing through
 * `addMessage` inside it, so nothing added during a batch was ever visible to
 * the dedup check. Accumulating into a local array and matching against both
 * it and `ctx.existing` closes that hole without re-reading global state.
 */
export function mergeFirmwareMessages(
  raw: FirmwareMessageWire[],
  ctx: FirmwareMergeContext,
): Message[] {
  const accepted: Message[] = [];
  raw.forEach((m, ringIndex) => {
    const fromAddr = parseAddr(m.from);
    const toAddr = parseAddr(m.to);
    const dir = m.direction;
    const isOutgoing = dir === 'outgoing' || dir === 'broadcast_out';
    const rawChannel = m.channelIndex ?? m.channel;
    const channelIndex = rawChannel !== undefined && rawChannel >= 0 ? rawChannel : undefined;
    const isBroadcast =
      dir === 'broadcast_in' ||
      dir === 'broadcast_out' ||
      (channelIndex === undefined && toAddr === 0xFFFFFFFF);
    // Skip self-addressed messages (firmware bug: old messages stored with wrong dest)
    if (!isBroadcast && fromAddr === toAddr && fromAddr === ctx.myAddr) return;
    // Convert uptime-based timestamp to wall clock: now - (uptime - msg_time)
    const msgUptimeS = m.timestamp_s ?? 0;
    const wallMs = ctx.deviceUptime > 0 && msgUptimeS > 0
      ? ctx.now - (ctx.deviceUptime - msgUptimeS) * 1000
      : ctx.now;
    /* `timestamp_s` is whole seconds, so a same-second burst from one peer used
     * to collapse onto a single synthetic id and addMessage silently dropped
     * all but the first. handle_get_messages walks the ring in order, so the
     * row's position in the response is the only disambiguator the firmware
     * payload actually carries: fold it in. Re-fetches shift these indices when
     * the ring rotates, but isLikelyDuplicate catches those on content. */
    const fallbackId = `fw-${msgUptimeS || ctx.now}-${fromAddr}-${ringIndex}`;
    const fwMsg: Message = {
      id: m.msgId ?? fallbackId,
      direction: isOutgoing ? 'outgoing' : 'incoming',
      from: fromAddr,
      to: isBroadcast ? 0xFFFFFFFF : toAddr,
      text: m.text,
      // Firmware rows do not carry a tier; Message declares it required but
      // this path has always stored undefined for firmware rows, so the cast
      // preserves that rather than inventing a default.
      tier: m.tier as MessageTier,
      channelIndex: isBroadcast ? undefined : channelIndex,
      timestampMs: wallMs,
      /* This path has always collapsed every firmware status to 'delivered':
       * a settled history fetch has no live ack timer behind it, so finer
       * fidelity for sent/failed isn't tracked here. 'queued' is the one
       * exception that must survive: it means the node parked the message
       * because the peer is offline, and labelling that 'delivered' would
       * claim the opposite of what actually happened. */
      status: m.status === 'queued' ? 'parked' : 'delivered',
    };
    /* Dedup against the store only, never within the batch. isLikelyDuplicate
     * is a content match, so an intra-batch check would drop a user genuinely
     * sending "ok" twice a few seconds apart, since both rows ride in one
     * response. msg_store_get maps index 0..count-1 onto distinct ring slots,
     * so a single response cannot repeat a stored message anyway and there is
     * nothing for such a check to catch. The ctx.existing check IS needed: a
     * re-poll returns the same rows with shifted ring indices, hence different
     * synthetic ids, and only content matching recognizes them. */
    // A match means the cached copy is at least as rich (it may carry relay
    // path or status from the web-side send path), so keep it and drop this one.
    if (ctx.existing.some(ex => isLikelyDuplicate(ex, fwMsg))) return;
    accepted.push(fwMsg);
  });
  return accepted;
}

export async function loadMessages(): Promise<void> {
  if (!session.client) return;
  // bramble.getMessages takes no params (EmptyParams in the contract): the
  // firmware serializes its whole ring buffer regardless, so send nothing.
  const result = await session.client.rpc<{ messages: FirmwareMessageWire[] }>(
    'bramble.getMessages',
    undefined,
    10000, // longer timeout: serializing the ring buffer can be slow on ESP32
  );
  const store = useStore.getState();
  const newFromFirmware = mergeFirmwareMessages(result.messages ?? [], {
    existing: store.messages,
    deviceUptime: store.status?.uptimeSec ?? 0,
    myAddr: store.config?.identity?.address ?? 0,
    now: Date.now(),
  });
  for (const msg of newFromFirmware) store.addMessage(msg);
  // Persist newly fetched messages to IndexedDB so they survive reconnects
  if (newFromFirmware.length > 0) {
    await messageDb.saveMessages(newFromFirmware).catch(() => {});
  }
}

// ─── Messaging ────────────────────────────────────────────────────────────

const packetIdToMsgId = new Map<string, string>();
const broadcastIdToMsgId = new Map<string, string>();
const pendingBroadcastTelemetry = new Map<string, BroadcastDeliveryNotification[]>();

/* Task 7 (audit gap #8): 'timeout' is a client-only UI status, distinct from
 * firmware's 'failed' (which only fires after the node exhausts its own
 * retry budget, up to ~15s for Normal tier and several minutes for Critical
 * -- docs/bramble-protocol-spec.md section 7.4). It exists purely to tell the
 * user "no confirmation yet" well before that, without asserting the send
 * failed. Firmware never reports 'timeout'; this map is how the UI produces
 * it and clears it again the moment a real ack/failure arrives. */
const sentStatusTimers = new Map<string, ReturnType<typeof setTimeout>>();
const SENT_TO_TIMEOUT_UI_MS = 10000;

function clearSentStatusTimer(msgId: string): void {
  const timer = sentStatusTimers.get(msgId);
  if (timer !== undefined) {
    clearTimeout(timer);
    sentStatusTimers.delete(msgId);
  }
}

function clearAllSentStatusTimers(): void {
  for (const timer of sentStatusTimers.values()) clearTimeout(timer);
  sentStatusTimers.clear();
}

function scheduleSentStatusTimeout(msgId: string): void {
  clearSentStatusTimer(msgId);
  const timer = setTimeout(() => {
    sentStatusTimers.delete(msgId);
    /* Only flip if still 'sent': a real ack/failed already superseded this,
     * or would have cleared the timer via clearSentStatusTimer. Re-checked
     * here too since the timer callback and an in-flight ack can race. */
    const current = useStore.getState().messages.find(m => m.id === msgId);
    if (current && current.status === 'sent') {
      useStore.getState().updateMessageStatus(msgId, 'timeout');
      messageDb.updateMessageStatus(msgId, 'timeout').catch(() => {});
    }
  }, SENT_TO_TIMEOUT_UI_MS);
  sentStatusTimers.set(msgId, timer);
}

const DEFAULT_DELIVERY_EVENT_RETENTION_DAYS = 30;
const DELIVERY_EVENT_RETENTION_DAYS = Number(import.meta.env.VITE_DELIVERY_EVENT_RETENTION_DAYS ?? DEFAULT_DELIVERY_EVENT_RETENTION_DAYS);
const DELIVERY_EVENT_SYNC_SEQ_KEY_PREFIX = 'bramble:delivery-event-sync:last-seq:';

interface DeliveryReplayEventWire {
  eventId?: string;
  event_id?: string;
  eventSeq?: number;
  event_seq?: number;
  messageId?: string | number;
  message_id?: string | number;
  packetId?: string | number;
  packet_id?: string | number;
  broadcastId?: string | number;
  broadcast_id?: string | number;
  eventType?: string;
  event_type?: string;
  ts?: number;
  timestampMs?: number;
  timestamp_ms?: number;
  payload?: unknown;
  data?: unknown;
}

interface DeliveryReplayResponse {
  events?: DeliveryReplayEventWire[];
  latestEventSeq?: number;
  latest_event_seq?: number;
}

function retentionCutoffTs(nowMs = Date.now()): number {
  const days = Number.isFinite(DELIVERY_EVENT_RETENTION_DAYS) && DELIVERY_EVENT_RETENTION_DAYS > 0
    ? DELIVERY_EVENT_RETENTION_DAYS
    : DEFAULT_DELIVERY_EVENT_RETENTION_DAYS;
  return nowMs - days * 24 * 60 * 60 * 1000;
}

function currentNodeAddrHex(): string {
  const addr = useStore.getState().config?.identity?.address;
  return addr != null ? formatAddrHex(addr) : 'default';
}

function lastDeliverySeqKey(nodeAddr: string): string {
  return `${DELIVERY_EVENT_SYNC_SEQ_KEY_PREFIX}${nodeAddr}`;
}

function loadLastDeliveryEventSeq(nodeAddr: string): number {
  const raw = safeGetItem(lastDeliverySeqKey(nodeAddr));
  const parsed = raw ? Number(raw) : 0;
  return Number.isFinite(parsed) && parsed >= 0 ? parsed : 0;
}

function saveLastDeliveryEventSeq(nodeAddr: string, seq: number): void {
  if (!Number.isFinite(seq) || seq < 0) return;
  safeSetItem(lastDeliverySeqKey(nodeAddr), String(Math.floor(seq)));
}

function asNumber(v: unknown): number | undefined {
  if (typeof v === 'number' && Number.isFinite(v)) return v;
  if (typeof v === 'string' && v.trim() !== '') {
    const n = Number(v);
    return Number.isFinite(n) ? n : undefined;
  }
  return undefined;
}

function normalizeReplayDeliveryEvent(raw: DeliveryReplayEventWire): DeliveryEventRecord | null {
  const eventType = String(raw.eventType ?? raw.event_type ?? '').trim() || 'unknown';
  const ts = asNumber(raw.ts ?? raw.timestampMs ?? raw.timestamp_ms) ?? Date.now();

  const packetIdRaw = raw.packetId ?? raw.packet_id;
  const broadcastIdRaw = raw.broadcastId ?? raw.broadcast_id;
  const packetId = packetIdRaw !== undefined && packetIdRaw !== null ? String(packetIdRaw) : '';
  const broadcastId = broadcastIdRaw !== undefined && broadcastIdRaw !== null ? String(broadcastIdRaw) : '';

  const messageIdRaw = raw.messageId ?? raw.message_id;
  let messageId = messageIdRaw !== undefined && messageIdRaw !== null ? String(messageIdRaw) : '';

  if (!messageId && packetId) {
    messageId = packetIdToMsgId.get(packetId) ?? '';
  }
  if (!messageId && broadcastId) {
    messageId = broadcastIdToMsgId.get(broadcastId) ?? '';
  }
  if (!messageId) return null;

  const seq = asNumber(raw.eventSeq ?? raw.event_seq);
  const payload = raw.payload ?? raw.data;

  const eventId = String(raw.eventId ?? raw.event_id ?? (seq !== undefined
    ? `replay:${seq}:${messageId}`
    : `replay:${eventType}:${messageId}:${ts}`));

  return {
    eventId,
    messageId,
    packetId: packetId || undefined,
    ts,
    eventType,
    payload,
  };
}

export async function syncDeliveryEventReplay(): Promise<void> {
  if (!session.client) return;

  let supportsDeliveryEventSync = false;
  try {
    const version = await session.client.rpc<Record<string, unknown>>('bramble.getVersion');
    supportsDeliveryEventSync = Boolean(
      version.supportsDeliveryEventSync ?? version.supports_delivery_event_sync,
    );
  } catch {
    return;
  }
  if (!supportsDeliveryEventSync) return;

  const nodeAddr = currentNodeAddrHex();
  const sinceEventSeq = loadLastDeliveryEventSeq(nodeAddr);

  let replay: DeliveryReplayResponse;
  try {
    replay = await session.client.rpc<DeliveryReplayResponse>('bramble.getDeliveryEvents', { sinceEventSeq });
  } catch (error) {
    if (isUnknownMethodError(error)) return;
    replay = await session.client.rpc<DeliveryReplayResponse>('bramble.getDeliveryEvents', { since_event_seq: sinceEventSeq });
  }

  const events = (replay.events ?? [])
    .map(normalizeReplayDeliveryEvent)
    .filter((e): e is DeliveryEventRecord => Boolean(e))
    .sort((a, b) => a.ts - b.ts);

  if (events.length > 0) {
    await deliveryEventStore.upsertDeliveryEvents(events);

    const eventsByMessage = new Map<string, DeliveryEventRecord[]>();
    for (const event of events) {
      const list = eventsByMessage.get(event.messageId) ?? [];
      list.push(event);
      eventsByMessage.set(event.messageId, list);
    }

    useStore.setState((state) => ({
      messages: state.messages.map((message) => {
        const applicable = eventsByMessage.get(message.id);
        if (!applicable || applicable.length === 0) return message;
        return applicable.reduce((acc, event) => applyDeliveryEventToMessage(acc, event), message);
      }),
    }));
  }

  const replayLatest = asNumber(replay.latestEventSeq ?? replay.latest_event_seq);
  const maxSeen = events.reduce((max, event) => {
    const seqMatch = /replay:(\d+):/.exec(event.eventId);
    const seq = seqMatch ? Number(seqMatch[1]) : undefined;
    return seq !== undefined && Number.isFinite(seq) ? Math.max(max, seq) : max;
  }, sinceEventSeq);
  saveLastDeliveryEventSeq(nodeAddr, Math.max(sinceEventSeq, replayLatest ?? 0, maxSeen));
}

function hydrateCorrelationMaps(messages: Message[]): void {
  packetIdToMsgId.clear();
  broadcastIdToMsgId.clear();
  pendingBroadcastTelemetry.clear();
  clearAllSentStatusTimers();
  for (const msg of messages) {
    if (msg.packetId) packetIdToMsgId.set(String(msg.packetId), msg.id);
    if (msg.broadcastId) broadcastIdToMsgId.set(msg.broadcastId, msg.id);
  }
}

function applyDeliveryEventToMessage(message: Message, event: DeliveryEventRecord): Message {
  if (event.eventType === 'ack') {
    const payload = (event.payload ?? {}) as { status?: 'delivered' | 'failed' | 'sent' | 'sending'; relayPath?: RelayHop[] };
    return {
      ...message,
      status: payload.status ?? message.status,
      relayPath: payload.relayPath ?? message.relayPath,
    };
  }

  if (event.eventType === 'broadcast_delivery') {
    const payload = (event.payload ?? {}) as {
      addr?: number;
      status?: 'delivered' | 'failed';
      hopCount?: number;
      deliveredAtMs?: number;
    };
    if (payload.addr === undefined || !payload.status) return message;
    const existing = message.broadcastRecipients ?? [];
    const idx = existing.findIndex(r => r.addr === payload.addr);
    const incoming = {
      addr: payload.addr,
      status: payload.status,
      hopCount: payload.hopCount ?? (idx >= 0 ? existing[idx].hopCount : 0),
      deliveredAtMs: payload.deliveredAtMs ?? event.ts,
    };
    const merged = mergeBroadcastRecipient(existing, incoming);
    return merged === existing ? message : { ...message, broadcastRecipients: merged };
  }

  return message;
}

async function hydrateMessagesWithDeliveryEvents(messages: Message[]): Promise<Message[]> {
  const hydrated = await Promise.all(messages.map(async (message) => {
    let events = await deliveryEventStore.listByMessage(message.id);
    if (events.length === 0 && message.packetId !== undefined && message.packetId !== null) {
      events = await deliveryEventStore.listByPacketId(String(message.packetId));
    }
    return events.reduce((acc, event) => applyDeliveryEventToMessage(acc, event), message);
  }));
  return hydrated;
}

interface BroadcastDeliveryNotification {
  broadcastId: string;
  packetId?: string;
  from: string | number;
  status: 'delivered' | 'pending' | 'failed';
  hopCount: number;
  deliveredAtMs: number;
  // firmware snake_case compatibility
  broadcast_id?: string;
  recipient?: string | number;
  packet_id?: string;
  hop_count?: number;
  delivered_at_ms?: number;
}

export function registerBroadcastSendTelemetry(msgId: string, meta: { packetId?: string; broadcastId?: string }): void {
  const { packetId, broadcastId } = meta;
  if (packetId || broadcastId) {
    useStore.getState().updateMessageBroadcastMeta(msgId, { packetId, broadcastId });
  }
  if (packetId) {
    packetIdToMsgId.set(packetId, msgId);
  }
  if (broadcastId) {
    broadcastIdToMsgId.set(broadcastId, msgId);
    applyPendingBroadcastTelemetry(broadcastId);
  }
}

function applyPendingBroadcastTelemetry(broadcastId: string): void {
  const queued = pendingBroadcastTelemetry.get(broadcastId);
  if (!queued || queued.length === 0) return;
  pendingBroadcastTelemetry.delete(broadcastId);
  for (const event of queued) {
    applyBroadcastDelivery(event);
  }
}

function applyBroadcastDelivery(event: BroadcastDeliveryNotification): void {
  const msgId = broadcastIdToMsgId.get(event.broadcastId);
  if (!msgId) {
    const existing = pendingBroadcastTelemetry.get(event.broadcastId) ?? [];
    pendingBroadcastTelemetry.set(event.broadcastId, [...existing, event]);
    return;
  }

  const recipient = {
    addr: parseAddr(event.from),
    status: event.status,
    hopCount: event.hopCount ?? 0,
    deliveredAtMs: event.deliveredAtMs ?? Date.now(),
  };

  deliveryEventStore.upsertDeliveryEvent({
    eventId: `broadcast:${event.broadcastId}:${recipient.addr}`,
    messageId: msgId,
    ts: recipient.deliveredAtMs,
    eventType: 'broadcast_delivery',
    payload: recipient,
  }).catch(() => {});

  useStore.getState().mergeBroadcastDeliveryRecipient(event.broadcastId, recipient);
}

export function handleBroadcastDelivery(params: unknown): void {
  const p = params as Partial<BroadcastDeliveryNotification>;
  const broadcastId = p.broadcastId ?? p.broadcast_id;
  const from = p.from ?? p.recipient;
  const packetId = p.packetId ?? p.packet_id;
  const hopCount = p.hopCount ?? p.hop_count ?? 0;
  const deliveredAtMs = p.deliveredAtMs ?? p.delivered_at_ms ?? Date.now();
  if (!broadcastId || !p.status || from === undefined) return;
  applyBroadcastDelivery({
    broadcastId,
    packetId,
    from,
    status: p.status,
    hopCount,
    deliveredAtMs,
  });
}

export async function sendMessage(
  dest: number,
  text: string,
  tier: MessageTier = 'normal',
  channelIndex?: number
): Promise<void> {
  const client = requireClient();
  const store = useStore.getState();

  const messageBytes = utf8Length(text);
  if (messageBytes > FRAGMENTED_MAX_BYTES) {
    throw new Error(`Message too long (${messageBytes} bytes). Max is ${FRAGMENTED_MAX_BYTES} bytes.`);
  }

  const fallbackAddr = (() => {
    const raw = safeGetItem(LAST_NODE_ADDR_KEY);
    if (!raw) return undefined;
    const parsed = parseInt(raw, 16);
    return Number.isFinite(parsed) ? parsed : undefined;
  })();
  const myAddr = store.config?.identity?.address ?? fallbackAddr ?? 0;
  const msg = {
    id: uuid(),
    direction: 'outgoing' as const,
    from: myAddr,
    to: dest,
    text,
    tier,
    channelIndex,
    timestampMs: Date.now(),
    status: 'sending' as const,
  };

  store.addMessage(msg);
  messageDb.saveMessage(msg).catch(() => {});

  try {
    const isChannelScoped = channelIndex !== undefined && channelIndex >= 0;
    const isBroadcast = dest === 0xFFFFFFFF && !isChannelScoped;
    const method = isBroadcast ? 'bramble.sendBroadcast' : 'bramble.sendMessage';
    const wireDest = (dest === 0xFFFFFFFE) ? 0xFFFFFFFF : dest;
    const params = isBroadcast
      ? { text }
      : {
          dest: formatAddrHex(wireDest),
          text,
          ...(isChannelScoped ? { channel: channelIndex } : {}),
        };
    const result = await client.rpc<{
      message_id?: string;
      status?: string;
      packetId?: string;
      packet_id?: string;
      broadcastId?: string;
      broadcast_id?: string;
      fragmented?: boolean;
      fragments_total?: number;
    }>(method, params);
    
    // Log fragmentation info for debugging
    if (result?.fragmented && result?.fragments_total) {
      console.info(`[send] Message fragmented into ${result.fragments_total} packets`);
    }
    
    store.updateMessageStatus(msg.id, 'sent');
    messageDb.updateMessageStatus(msg.id, 'sent').catch(() => {});
    registerBroadcastSendTelemetry(msg.id, {
      packetId: result?.packetId ?? result?.packet_id,
      broadcastId: result?.broadcastId ?? result?.broadcast_id,
    });
    /* Unicast only: broadcasts have no single ack and are tracked instead
     * via broadcastRecipients/telemetry, which already has its own
     * per-recipient pending/delivered/failed state. */
    if (!isBroadcast) {
      scheduleSentStatusTimeout(msg.id);
    }
  } catch (e) {
    clearSentStatusTimer(msg.id);
    store.updateMessageStatus(msg.id, 'failed');
    messageDb.updateMessageStatus(msg.id, 'failed').catch(() => {});
    throw e;
  }
}

// ─── Notification handlers ────────────────────────────────────────────────

export function handleAck(params: unknown): void {
  const p = params as Record<string, unknown>;
  /* Firmware sends snake_case (packet_id), webapp convention is camelCase */
  const packetId = (p.packetId ?? p.packet_id) as string | undefined;
  const status = (p.status as string) ?? 'delivered';
  /* Normalize relayPath: firmware sends addr as hex string, webapp needs number */
  const rawPath = p.relayPath as Array<{ addr: string | number; rssi: number }> | undefined;
  const relayPath: RelayHop[] | undefined = rawPath?.map(hop => ({
    addr: parseAddr(hop.addr),
    rssi: hop.rssi ?? 0,
  }));

  if (!packetId) return;
  const msgId = packetIdToMsgId.get(packetId);
  if (msgId) {
    packetIdToMsgId.delete(packetId);
    clearSentStatusTimer(msgId);
    const newStatus = status === 'delivered' ? 'delivered' : 'failed';
    const nowTs = Date.now();

    deliveryEventStore.upsertDeliveryEvent({
      eventId: `ack:${packetId}:${newStatus}`,
      messageId: msgId,
      packetId,
      ts: nowTs,
      eventType: 'ack',
      payload: { status: newStatus, relayPath },
    }).catch(() => {});

    useStore.getState().updateMessageStatus(msgId, newStatus, relayPath);
    messageDb.updateMessageStatus(msgId, newStatus, relayPath).catch(() => {});
  }
}

// The bramble.onMessage push payload: a firmware message row plus the
// sender's display name when the firmware's neighbor table knows it. Push
// notifications are not part of the OpenAPI request/response contract, so the
// extras beyond the Message schema are declared here.
type IncomingRealtimeWire = FirmwareMessageWire & {
  fromName?: string;
};

export function normalizeIncomingRealtimeMessage(params: unknown) {
  const p = params as IncomingRealtimeWire;
  const fromAddr = parseAddr(p.from);
  const toAddr = parseAddr(p.to);
  const rawChannel = p.channelIndex ?? (p.channel as number | undefined);
  const channelIndex = rawChannel !== undefined && rawChannel >= 0 ? rawChannel : undefined;
  const isBroadcast = channelIndex === undefined && (p.broadcast === true || toAddr === 0xFFFFFFFF);

  return {
    id: p.msgId ?? `rt-${Date.now()}`,
    direction: 'incoming' as const,
    from: fromAddr,
    to: isBroadcast ? 0xFFFFFFFF : toAddr,
    text: p.text,
    // Same as mergeFirmwareMessages: pushes may omit tier and this path has
    // always stored undefined then, so the cast preserves that.
    tier: p.tier as MessageTier,
    channelIndex,
    timestampMs: Date.now(),
    status: 'delivered' as const,
  };
}

export function handleIncomingMessage(params: unknown): void {
  const p = params as IncomingRealtimeWire;
  const msg = normalizeIncomingRealtimeMessage(p);
  const store = useStore.getState();
  // The firmware includes the sender's display name when its neighbor table
  // knows it. Learn it BEFORE rendering anything (chat list, notification),
  // so a first message from a peer never shows as a bare hex address.
  if (typeof p.fromName === 'string' && p.fromName.length > 0) {
    store.setPeerName(msg.from, p.fromName);
  }
  store.addMessage(msg);
  messageDb.saveMessage(msg).catch(() => {});
  maybeNotifyIncoming(msg);
}

/**
 * Raise a native Android notification for an incoming message. No-op outside
 * the Android shell (web and Electron have no bridge). Suppressed for a
 * message from this node and, following common messaging-app behavior, for
 * the conversation the user is currently looking at (app visible + that
 * conversation open).
 */
function maybeNotifyIncoming(msg: Message): void {
  if (!isAndroidShell()) return;
  const notify = window.brambleAndroidNotify;
  if (!notify) return;

  const store = useStore.getState();
  const selfAddr = store.config?.identity?.address;
  if (selfAddr !== undefined && msg.from === selfAddr) return;

  const conversationId = conversationIdForMessage(msg);
  const appVisible = typeof document !== 'undefined' && document.visibilityState === 'visible';
  if (appVisible && store.activeConversationId === conversationId) return;

  const senderName = store.peerNames.get(msg.from) ?? formatAddr0x(msg.from);
  // Title the notification with the store's canonical conversation label so it
  // matches the chat header and sidebar; for an incoming DM this resolves to the
  // same peer name as senderName.
  const conversationTitle = formatConversationLabel(conversationId, store.peerNames, store.config);

  const payload: NativeMessageNotification = {
    conversationId,
    conversationTitle,
    sender: senderName,
    text: msg.text ?? '',
    timestamp: msg.timestampMs ?? Date.now(),
  };

  try {
    notify.onMessage(JSON.stringify(payload));
  } catch { /* notification is best-effort */ }
}

export function openDM(addr: number): void {
  const store = useStore.getState();
  store.setActiveConversation(`dm:${addr}`);
  store.setActiveTab('chat');
}

// ─── Probe / Network Reach ────────────────────────────────────────────────

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

// Clears every module-level correlation singleton (the packet/broadcast id
// maps, the pending broadcast telemetry, and the sent-status timers). Shared by
// the test-reset entry points so a new correlation map only has to be added
// here, not in each of them.
function resetCorrelationState(): void {
  packetIdToMsgId.clear();
  broadcastIdToMsgId.clear();
  pendingBroadcastTelemetry.clear();
  clearAllSentStatusTimers();
}

export function __resetBroadcastTelemetryForTests(): void {
  resetCorrelationState();
}

// Test isolation: actions.ts holds several module-level singletons (the
// transport client, the packet/broadcast correlation maps, the pending
// sent-status timers). Nothing resets them between test files, so state can
// leak across suites once restoreMocks/afterEach hygiene is in place. Call
// this from test/setup.ts's afterEach.
export function __resetActionsForTests(): void {
  session.client = null;
  resetCorrelationState();
}

export function __normalizeReplayDeliveryEventForTests(raw: DeliveryReplayEventWire): DeliveryEventRecord | null {
  return normalizeReplayDeliveryEvent(raw);
}

export function __clearDeliveryEventSyncStateForTests(nodeAddr?: string): void {
  resetCorrelationState();

  try {
    if (nodeAddr) {
      localStorage.removeItem(lastDeliverySeqKey(nodeAddr));
      return;
    }
    const keys: string[] = [];
    for (let i = 0; i < localStorage.length; i += 1) {
      const key = localStorage.key(i);
      if (key?.startsWith(DELIVERY_EVENT_SYNC_SEQ_KEY_PREFIX)) keys.push(key);
    }
    for (const key of keys) {
      localStorage.removeItem(key);
    }
  } catch {
    // noop
  }
}
