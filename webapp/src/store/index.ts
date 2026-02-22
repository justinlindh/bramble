import { create } from 'zustand';
import type {
  AppState,
  Message,
  Neighbor,
  Route,
  BrambleConfig,
  NodeStatus,
  AirtimeStatus,
  AirtimePolicy,
  ConnectionState,
  RelayHop,
  DeliveryStatus,
  Transport,
  ProbeResult,
  PeerLocation,
  TrafficDebugStatus,
  TrafficEvent,
} from '../types/bramble';
import { saveUnreadCounts, loadUnreadCounts } from './unreadStore';

const ROUTE_VISIBILITY_KEY = 'bramble_show_routes';

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

function formatAddr(id: string, peerNames?: Map<number, string>, config?: BrambleConfig | null): string {
  if (id === 'broadcast') return 'Broadcast';
  if (id.startsWith('ch:')) {
    const idx = Number(id.slice(3));
    const ch = config?.channels?.find(c => c.index === idx);
    return ch?.name?.trim() ? ch.name : `ch-${idx}`;
  }
  if (id.startsWith('dm:')) {
    const addr = Number(id.slice(3));
    const name = peerNames?.get(addr);
    if (name) return name;
    return `0x${addr.toString(16).toUpperCase()}`;
  }
  return id;
}

function persistUnreads(conversations: Map<string, any>, config: BrambleConfig | null): void {
  if (!config?.identity?.address) return;
  const nodeAddr = config.identity.address.toString(16).toUpperCase().padStart(8, '0');
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
  setTransport: (t: Transport | null) => void;
  setConfig: (c: BrambleConfig) => void;
  setStatus: (s: NodeStatus) => void;
  setAirtime: (a: AirtimeStatus) => void;
  setAirtimePolicy: (p: AirtimePolicy) => void;
  setNeighbors: (n: Neighbor[]) => void;
  setRoutes: (r: Route[]) => void;
  addMessage: (msg: Message) => void;
  updateMessageStatus: (id: string, status: DeliveryStatus, relayPath?: RelayHop[]) => void;
  setActiveConversation: (id: string) => void;
  activeTab: string;
  setActiveTab: (tab: string) => void;
  showRoutes: boolean;
  setShowRoutes: (show: boolean) => void;
  setProbeResult: (r: ProbeResult | null) => void;
  setProbeCollecting: (c: boolean) => void;
  setPeerLocations: (locs: PeerLocation[]) => void;
  setMapFocusAddr: (addr: number | null) => void;
  loadCachedMessages: (msgs: Message[]) => void;
  peerNames: Map<number, string>;
  resetNodeData: () => void;
  setTrafficDebugStatus: (s: TrafficDebugStatus) => void;
  addTrafficEvent: (e: TrafficEvent) => void;
  addTrafficEvents: (events: TrafficEvent[]) => void;
  clearTrafficEvents: () => void;
}

export const useStore = create<AppState & Actions>((set) => ({
  // ─── Initial state ───────────────────────────────────────────────────
  connectionState: 'disconnected',
  connectionError: undefined,
  transport: null,
  config: null,
  status: null,
  airtime: null,
  airtimePolicy: null,
  neighbors: [],
  routes: [],
  messages: [],
  conversations: new Map(),
  activeConversationId: 'broadcast',
  activeTab: 'chat',
  showRoutes: loadShowRoutes(),
  probeResult: null,
  peerNames: new Map(),
  probeCollecting: false,
  peerLocations: [],
  mapFocusAddr: null,
  trafficDebugStatus: null,
  trafficEvents: [],

  // ─── Actions ─────────────────────────────────────────────────────────
  setConnectionState: (s, err?) =>
    set({ connectionState: s, connectionError: err }),

  setTransport: (t) => set({ transport: t }),

  setConfig: (c) => set(state => {
    const names = new Map(state.peerNames);
    if (c.identity?.name && c.identity.name !== '(unnamed)') {
      names.set(c.identity.address, c.identity.name);
    }

    const convs = new Map(state.conversations);
    for (const [id, conv] of convs) {
      if (id.startsWith('ch:')) {
        convs.set(id, { ...conv, label: formatAddr(id, names, c) });
      }
    }

    return { config: c, peerNames: names, conversations: convs };
  }),

  setStatus: (s) => set({ status: s }),

  setAirtime: (a) => set({ airtime: a }),

  setAirtimePolicy: (p) => set({ airtimePolicy: p }),

  setNeighbors: (n) => set(state => {
    const names = new Map(state.peerNames);
    for (const nb of n) {
      if ((nb as any).name) names.set(nb.addr, (nb as any).name);
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

      // Determine conversation ID
      // Broadcasts (to === 0xFFFFFFFF) always file under 'broadcast', not a DM,
      // to avoid double-showing them in both the broadcast view and sender's DM.
      const isBroadcast = msg.to === 0xffffffff || msg.channelIndex === -1;
      const convId =
        msg.channelIndex !== undefined && msg.channelIndex >= 0
          ? `ch:${msg.channelIndex}`
          : isBroadcast
          ? 'broadcast'
          : `dm:${msg.direction === 'outgoing' ? msg.to : msg.from}`;

      // Update conversation summary
      const convs = new Map(state.conversations);
      const prev = convs.get(convId);
      
      // Only increment unread count if:
      // 1. Message is incoming, AND
      // 2. This conversation is NOT currently active
      const isActive = state.activeConversationId === convId;
      const shouldIncrementUnread = msg.direction === 'incoming' && !isActive;
      
      convs.set(convId, {
        id: convId,
        label: formatAddr(convId, state.peerNames, state.config),
        peerAddr:
          msg.channelIndex !== undefined || isBroadcast
            ? undefined
            : msg.direction === 'outgoing'
            ? msg.to
            : msg.from,
        channelIndex: msg.channelIndex,
        lastMessage: msg.text.slice(0, 60),
        lastMessageTime: msg.timestampMs,
        unreadCount:
          (prev?.unreadCount ?? 0) +
          (shouldIncrementUnread ? 1 : 0),
      });

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

  setActiveTab: (tab: string) => set({ activeTab: tab }),
  setShowRoutes: (show: boolean) => {
    saveShowRoutes(show);
    set({ showRoutes: show });
  },

  resetNodeData: () => set({
    messages: [],
    conversations: new Map(),
    neighbors: [],
    routes: [],
    peerNames: new Map(),
    config: null as any,
    status: null as any,
    airtime: null as any,
    probeResult: null,
    probeCollecting: false,
    peerLocations: [],
    mapFocusAddr: null,
  }),

  setProbeResult: (r) => set({ probeResult: r }),
  setProbeCollecting: (c) => set({ probeCollecting: c }),

  setPeerLocations: (locs) => set({ peerLocations: locs }),
  setMapFocusAddr: (addr) => set({ mapFocusAddr: addr }),

  loadCachedMessages: (msgs: Message[]) =>
    set(state => {
      // Load persisted unread counts from localStorage
      const nodeAddr = state.config?.identity?.address?.toString(16).toUpperCase().padStart(8, '0');
      const savedUnreads = loadUnreadCounts(nodeAddr);

      // Rebuild conversations from cached messages
      const convs = new Map(state.conversations);
      for (const msg of msgs) {
        const isBroadcast = msg.to === 0xffffffff || msg.channelIndex === -1;
        const convId =
          msg.channelIndex !== undefined && msg.channelIndex >= 0
            ? `ch:${msg.channelIndex}`
            : isBroadcast
            ? 'broadcast'
            : `dm:${msg.direction === 'outgoing' ? msg.to : msg.from}`;
        const prev = convs.get(convId);
        const shouldUpdate = !prev || !prev.lastMessageTime || msg.timestampMs > prev.lastMessageTime;
        if (shouldUpdate) {
          convs.set(convId, {
            id: convId,
            label: formatAddr(convId, state.peerNames, state.config),
            peerAddr:
              msg.channelIndex !== undefined || isBroadcast
                ? undefined
                : msg.direction === 'outgoing'
                ? msg.to
                : msg.from,
            channelIndex: msg.channelIndex,
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
      
      return { activeConversationId: id, conversations: convs };
    }),

  setTrafficDebugStatus: (s) => set({ trafficDebugStatus: s }),

  addTrafficEvent: (e) =>
    set(state => ({
      trafficEvents: [...state.trafficEvents, e].slice(-1000), // Keep last 1000 events
    })),

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

  clearTrafficEvents: () => set({ trafficEvents: [] }),
}));
