import { create } from 'zustand';
import type {
  AppState,
  Message,
  Neighbor,
  Route,
  BrambleConfig,
  NodeStatus,
  AirtimeStatus,
  ConnectionState,
  RelayHop,
  DeliveryStatus,
  ProbeResult,
  PeerLocation,
  PeerVerification,
  TrafficDebugStatus,
  TrafficEvent,
  ConnectionCapabilities,
  NetworkKeyStatus,
  AnchorStatus,
} from '../types/bramble';
import { saveUnreadCounts, loadUnreadCounts } from './unreadStore';
import type { SavedDevice } from '../lib/deviceBook';
import { formatAddrHex, formatAddr0x } from '../utils/address';
import { DEFAULT_CAPABILITIES } from '../lib/connectionMode';
import { mergeBroadcastRecipient } from '../lib/broadcastRecipients';

const ROUTE_VISIBILITY_KEY = 'bramble_show_routes';
const ACTIVE_TAB_KEY = 'bramble-active-tab';

// Verbose per-message tracing is off by default: it logs full message text and
// would leak content into the console. Enable with localStorage 'bramble:debug'='1'.
function debugLog(...args: unknown[]): void {
  try {
    if (localStorage.getItem('bramble:debug') === '1') console.debug(...args);
  } catch {
    // ignore storage access failures
  }
}

function loadShowRoutes(): boolean {
  try {
    return localStorage.getItem(ROUTE_VISIBILITY_KEY) === '1';
  } catch {
    return false;
  }
}

function saveShowRoutes(show: boolean): void {
  try {
    localStorage.setItem(ROUTE_VISIBILITY_KEY, show ? '1' : '0');
  } catch {
    // noop
  }
}

function loadActiveTab(): string {
  try {
    return localStorage.getItem(ACTIVE_TAB_KEY) || 'chat';
  } catch {
    return 'chat';
  }
}

function saveActiveTab(tab: string): void {
  try {
    localStorage.setItem(ACTIVE_TAB_KEY, tab);
  } catch {
    // noop
  }
}

// The conversation bucket a message belongs to, plus the peer and channel
// fields that must agree with it.
export interface ConversationTarget {
  id: string;
  // Set only for DM buckets: the other party's address. Channel and broadcast
  // buckets have no single peer.
  peerAddr: number | undefined;
  // Set only for channel buckets, and always non-negative there.
  channelIndex: number | undefined;
}

// Map a message to its conversation bucket, matching the Zustand store
// convention: channel -> 'ch:{index}', broadcast (to === 0xffffffff) ->
// 'broadcast', otherwise a DM keyed by the peer implied by direction.
// Broadcasts always file under 'broadcast' rather than the sender's DM so they
// are not double-shown in both views.
//
// The firmware's on-the-wire sentinel for "not a channel message" is a negative
// channel index (MSG_STORE_DM_CHANNEL === -1, see components/msg_store). Both
// webapp decode paths (loadMessages and normalizeIncomingRealtimeMessage in
// store/actions.ts) already normalize any negative index to undefined, so a
// Message reaching the store should never carry -1. The >= 0 test here is the
// belt-and-braces backstop for that, and returning id, peerAddr and
// channelIndex together is what keeps them from disagreeing: a bucket's kind is
// decided exactly once, here, instead of being re-derived by each caller.
function conversationTargetForMessage(msg: Message): ConversationTarget {
  if (msg.channelIndex !== undefined && msg.channelIndex >= 0) {
    return { id: `ch:${msg.channelIndex}`, peerAddr: undefined, channelIndex: msg.channelIndex };
  }
  if (msg.to === 0xffffffff) {
    return { id: 'broadcast', peerAddr: undefined, channelIndex: undefined };
  }
  const peerAddr = msg.direction === 'outgoing' ? msg.to : msg.from;
  return { id: `dm:${peerAddr}`, peerAddr, channelIndex: undefined };
}

// Exported for tests that pin the id/peerAddr/channelIndex agreement.
export const __conversationTargetForMessage = conversationTargetForMessage;

// Narrow accessor for callers that only need the conversation bucket id, such
// as delivery-event correlation and notification routing in store/actions.ts.
// It derives the id through the one classifier above so those paths cannot
// drift from the store the way earlier hand-rolled copies did (#124, #153,
// #168, #189).
export function conversationIdForMessage(msg: Message): string {
  return conversationTargetForMessage(msg).id;
}

// A conversation id decoded back into its kind and payload: the inverse of the
// 'broadcast' / 'ch:{index}' / 'dm:{addr}' scheme that conversationTargetForMessage
// produces. Decoding lives here alone, so no caller re-parses the id string by
// hand, which is what caused #124, #153, #168, and #189. (The one deliberate
// exception is messageDb's v1 migration, which detects the retired
// 'dm:{hex}-{hex}' format this parser never handled.)
export type ParsedConversationId =
  | { kind: 'broadcast' }
  | { kind: 'channel'; index: number }
  | { kind: 'dm'; addr: number }
  | { kind: 'unknown' };

export function parseConversationId(id: string): ParsedConversationId {
  if (id === 'broadcast') return { kind: 'broadcast' };
  if (id.startsWith('ch:')) return { kind: 'channel', index: Number(id.slice(3)) };
  if (id.startsWith('dm:')) return { kind: 'dm', addr: Number(id.slice(3)) };
  return { kind: 'unknown' };
}

// Display label for a conversation bucket. Exported so UI headers can render
// the same name policy the store uses when it labels conversations.
export function formatConversationLabel(id: string, peerNames?: Map<number, string>, config?: BrambleConfig | null): string {
  const parsed = parseConversationId(id);
  switch (parsed.kind) {
    case 'broadcast':
      return 'Broadcast';
    case 'channel': {
      const ch = config?.channels?.find(c => c.index === parsed.index);
      return ch?.name?.trim() ? ch.name : `ch-${parsed.index}`;
    }
    case 'dm': {
      const name = peerNames?.get(parsed.addr);
      return name ? name : formatAddr0x(parsed.addr);
    }
    case 'unknown':
      return id;
  }
}

function persistUnreads(conversations: Map<string, any>, config: BrambleConfig | null): void {
  if (!config?.identity?.address) return;
  const nodeAddr = formatAddrHex(config.identity.address);
  const counts: Record<string, number> = {};
  for (const [id, conv] of conversations) {
    if (conv.unreadCount > 0) {
      counts[id] = conv.unreadCount;
    }
  }
  saveUnreadCounts(nodeAddr, counts);
}

interface Actions {
  setConnectionState: (s: ConnectionState, err?: string) => void;
  setConnectionCapabilities: (c: ConnectionCapabilities) => void;
  setConfig: (c: BrambleConfig) => void;
  setStatus: (s: NodeStatus) => void;
  setAirtime: (a: AirtimeStatus) => void;
  setNeighbors: (n: Neighbor[]) => void;
  setRoutes: (r: Route[]) => void;
  addMessage: (msg: Message) => void;
  updateMessageStatus: (id: string, status: DeliveryStatus, relayPath?: RelayHop[]) => void;
  updateMessageBroadcastMeta: (id: string, patch: { packetId?: string | number; broadcastId?: string }) => void;
  mergeBroadcastDeliveryRecipient: (broadcastId: string, recipient: { addr: number; status: 'delivered' | 'pending' | 'failed'; hopCount: number; deliveredAtMs: number }) => void;
  setActiveConversation: (id: string) => void;
  activeTab: string;
  setActiveTab: (tab: string) => void;
  showRoutes: boolean;
  setShowRoutes: (show: boolean) => void;
  setProbeResult: (r: ProbeResult | null) => void;
  setDevices: (d: SavedDevice[]) => void;
  setPeerLocations: (locs: PeerLocation[]) => void;
  setMapFocusAddr: (addr: number | null) => void;
  setPeerVerification: (addr: number, v: PeerVerification) => void;
  loadCachedMessages: (msgs: Message[]) => void;
  peerNames: Map<number, string>;
  setPeerName: (addr: number, name: string) => void;
  resetNodeData: () => void;
  setTrafficDebugStatus: (s: TrafficDebugStatus) => void;
  addTrafficEvents: (events: TrafficEvent[]) => void;
  setNetworkKeyStatus: (s: NetworkKeyStatus | null) => void;
  setAnchorStatus: (s: AnchorStatus | null) => void;
}

export const useStore = create<AppState & Actions>((set) => ({
  // ─── Initial state ───────────────────────────────────────────────────
  connectionState: 'disconnected',
  connectionError: undefined,
  connectionCapabilities: DEFAULT_CAPABILITIES,
  capabilitiesLoaded: false,
  config: null,
  status: null,
  airtime: null,
  neighbors: undefined,
  routes: [],
  messages: [],
  conversations: new Map(),
  activeConversationId: 'broadcast',
  activeTab: loadActiveTab(),
  showRoutes: loadShowRoutes(),
  probeResult: null,
  peerNames: new Map(),
  devices: [],
  peerLocations: [],
  mapFocusAddr: null,
  peerVerifications: new Map(),
  trafficDebugStatus: null,
  trafficEvents: [],
  networkKeyStatus: null,
  anchorStatus: null,

  // ─── Actions ─────────────────────────────────────────────────────────
  setConnectionState: (s, err?) =>
    set({ connectionState: s, connectionError: err }),

  // Setting capabilities is what makes them known, whatever their source: the
  // /api/capabilities response, its failure fallback, or an embedded shell's
  // constants, which resolve with no network round trip.
  setConnectionCapabilities: (c) => set({ connectionCapabilities: c, capabilitiesLoaded: true }),

  setConfig: (c) => set(state => {
    const names = new Map(state.peerNames);
    if (c.identity?.name && c.identity.name !== '(unnamed)') {
      names.set(c.identity.address, c.identity.name);
    }

    // Build set of valid channel indexes from config
    const validChannelIndexes = new Set(c.channels?.map(ch => ch.index) ?? []);

    const convs = new Map(state.conversations);
    for (const [id, conv] of convs) {
      const parsed = parseConversationId(id);
      if (parsed.kind === 'channel') {
        if (!validChannelIndexes.has(parsed.index)) {
          // Channel was deleted: remove stale conversation (BUG-07 fix)
          convs.delete(id);
        } else {
          convs.set(id, { ...conv, label: formatConversationLabel(id, names, c) });
        }
      }
    }

    // If active conversation was a deleted channel, fall back to broadcast
    const activeId = state.activeConversationId;
    const activeGone = parseConversationId(activeId).kind === 'channel' && !convs.has(activeId);

    return {
      config: c,
      peerNames: names,
      conversations: convs,
      ...(activeGone ? { activeConversationId: 'broadcast' } : {}),
    };
  }),

  setStatus: (s) => set({ status: s }),

  setAirtime: (a) => set({ airtime: a }),

  setNeighbors: (n) => set(state => {
    const names = new Map(state.peerNames);
    for (const nb of n) {
      // normalizeNeighbor (store/actions/telemetry.ts) attaches the firmware's
      // display name when present; the store's Neighbor type does not carry it.
      const { name } = nb as Neighbor & { name?: string };
      if (name) names.set(nb.addr, name);
    }
    return { neighbors: n, peerNames: names };
  }),

  setRoutes: (r) => set({ routes: r }),

  addMessage: (msg: Message) =>
    set(state => {
      // Deduplicate by id
      if (state.messages.some(m => m.id === msg.id)) return state;
      // Cap message history at 500
      const msgs = [...state.messages, msg].slice(-500);

      const isBroadcast = msg.to === 0xffffffff;
      const target = conversationTargetForMessage(msg);
      const convId = target.id;

      debugLog('[addMessage] Message:', msg);
      debugLog('[addMessage] Determined convId:', convId, '| isBroadcast:', isBroadcast, '| activeConv:', state.activeConversationId);

      // Update conversation summary
      const convs = new Map(state.conversations);
      const prev = convs.get(convId);
      
      // Only increment unread count if:
      // 1. Message is incoming, AND
      // 2. This conversation is NOT currently active
      const isActive = state.activeConversationId === convId;
      const shouldIncrementUnread = msg.direction === 'incoming' && !isActive;
      
      const newConv = {
        id: convId,
        label: formatConversationLabel(convId, state.peerNames, state.config),
        peerAddr: target.peerAddr,
        channelIndex: target.channelIndex,
        lastMessage: msg.text.slice(0, 60),
        lastMessageTime: msg.timestampMs,
        unreadCount:
          (prev?.unreadCount ?? 0) +
          (shouldIncrementUnread ? 1 : 0),
      };
      
      debugLog('[addMessage] Creating conversation:', newConv);
      convs.set(convId, newConv);

      // Persist unread counts to localStorage
      persistUnreads(convs, state.config);

      return { messages: msgs, conversations: convs };
    }),

  updateMessageStatus: (id: string, status: DeliveryStatus, relayPath?: RelayHop[]) =>
    set(state => ({
      messages: state.messages.map(m =>
        m.id === id
          ? { ...m, status, relayPath: relayPath ?? m.relayPath }
          : m
      ),
    })),

  updateMessageBroadcastMeta: (id, patch) =>
    set(state => ({
      messages: state.messages.map(m => (m.id === id ? { ...m, ...patch } : m)),
    })),

  mergeBroadcastDeliveryRecipient: (broadcastId, recipient) =>
    set(state => ({
      messages: state.messages.map(m => {
        if (m.broadcastId !== broadcastId) return m;
        const existing = m.broadcastRecipients ?? [];
        const next = mergeBroadcastRecipient(existing, recipient);
        return next === existing ? m : { ...m, broadcastRecipients: next };
      }),
    })),

  setActiveTab: (tab: string) => {
    saveActiveTab(tab);
    set({ activeTab: tab });
  },
  setShowRoutes: (show: boolean) => {
    saveShowRoutes(show);
    set({ showRoutes: show });
  },

  setPeerName: (addr, name) => set(state => {
    const names = new Map(state.peerNames);
    if (name) {
      names.set(addr, name);
    } else {
      names.delete(addr);
    }
    // Update labels on any DM conversation for this peer. Route through the
    // shared labeler (reading the just-updated names map) instead of
    // re-deriving the DM label here, so this path cannot drift from the
    // classifier every other labeling path uses.
    const convs = new Map(state.conversations);
    const dmKey = `dm:${addr}`;
    const conv = convs.get(dmKey);
    if (conv) {
      convs.set(dmKey, { ...conv, label: formatConversationLabel(dmKey, names, state.config) });
    }
    return { peerNames: names, conversations: convs };
  }),

  resetNodeData: () => set({
    messages: [],
    conversations: new Map(),
    neighbors: undefined,
    routes: [],
    peerNames: new Map(),
    config: null as any,
    status: null as any,
    airtime: null as any,
    probeResult: null,
    peerLocations: [],
    mapFocusAddr: null,
    peerVerifications: new Map(),
    networkKeyStatus: null,
    anchorStatus: null,
  }),

  setProbeResult: (r) => set({ probeResult: r }),
  setDevices: (devices) => set({ devices }),

  setPeerLocations: (locs) => set({ peerLocations: locs }),
  setMapFocusAddr: (addr) => set({ mapFocusAddr: addr }),

  setPeerVerification: (addr, v) => set(state => {
    const next = new Map(state.peerVerifications);
    next.set(addr, v);
    return { peerVerifications: next };
  }),

  loadCachedMessages: (msgs: Message[]) =>
    set(state => {
      // Load persisted unread counts from localStorage
      const addr = state.config?.identity?.address;
      const nodeAddr = addr != null ? formatAddrHex(addr) : undefined;
      const savedUnreads = loadUnreadCounts(nodeAddr);

      // Rebuild conversations from cached messages
      const convs = new Map(state.conversations);
      for (const msg of msgs) {
        const target = conversationTargetForMessage(msg);
        const convId = target.id;
        const prev = convs.get(convId);
        const shouldUpdate = !prev || !prev.lastMessageTime || msg.timestampMs > prev.lastMessageTime;
        if (shouldUpdate) {
          convs.set(convId, {
            id: convId,
            label: formatConversationLabel(convId, state.peerNames, state.config),
            peerAddr: target.peerAddr,
            channelIndex: target.channelIndex,
            lastMessage: msg.text.slice(0, 60),
            lastMessageTime: msg.timestampMs,
            unreadCount: savedUnreads[convId] ?? 0,
          });
        }
      }
      return { messages: msgs.slice(-500), conversations: convs };
    }),

  setActiveConversation: (id: string) =>
    set(state => {
      const convs = new Map(state.conversations);
      const conv = convs.get(id);
      if (conv) convs.set(id, { ...conv, unreadCount: 0 });

      // Persist unread counts to localStorage
      persistUnreads(convs, state.config);

      // Opening a conversation dismisses its native notification (Android shell).
      try { window.brambleAndroidNotify?.clearConversation(id); } catch { /* best-effort */ }

      return { activeConversationId: id, conversations: convs };
    }),

  setTrafficDebugStatus: (s) => set({ trafficDebugStatus: s }),

  // Both the live push (one event) and the poll (a batch) land here, so the
  // live path gets the same seq-dedup and seq-ordering the poll relies on: a
  // duplicate seq replaces rather than appends, and the list stays sorted.
  addTrafficEvents: (events) =>
    set(state => {
      // Merge by seq, keeping newest
      const bySeq = new Map(state.trafficEvents.map(e => [e.seq, e]));
      for (const e of events) {
        bySeq.set(e.seq, e);
      }
      const merged = Array.from(bySeq.values()).sort((a, b) => a.seq - b.seq).slice(-1000);
      return { trafficEvents: merged };
    }),

  setNetworkKeyStatus: (s) => set({ networkKeyStatus: s }),
  setAnchorStatus: (s) => set({ anchorStatus: s }),
}));
