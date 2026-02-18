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
  Transport,
  ProbeResult,
  PeerLocation,
} from '../types/bramble';

function formatAddr(id: string, peerNames?: Map<number, string>): string {
  if (id === 'broadcast') return '📢 Broadcast';
  if (id.startsWith('ch:')) return `ch-${id.slice(3)}`;
  if (id.startsWith('dm:')) {
    const addr = Number(id.slice(3));
    const name = peerNames?.get(addr);
    if (name) return name;
    return `0x${addr.toString(16).toUpperCase()}`;
  }
  return id;
}

interface Actions {
  setConnectionState: (s: ConnectionState, err?: string) => void;
  setTransport: (t: Transport | null) => void;
  setConfig: (c: BrambleConfig) => void;
  setStatus: (s: NodeStatus) => void;
  setAirtime: (a: AirtimeStatus) => void;
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
}

export const useStore = create<AppState & Actions>((set) => ({
  // ─── Initial state ───────────────────────────────────────────────────
  connectionState: 'disconnected',
  connectionError: undefined,
  transport: null,
  config: null,
  status: null,
  airtime: null,
  neighbors: [],
  routes: [],
  messages: [],
  conversations: new Map(),
  activeConversationId: 'broadcast',
  activeTab: 'chat',
  showRoutes: false,
  probeResult: null,
  peerNames: new Map(),
  probeCollecting: false,
  peerLocations: [],
  mapFocusAddr: null,

  // ─── Actions ─────────────────────────────────────────────────────────
  setConnectionState: (s, err?) =>
    set({ connectionState: s, connectionError: err }),

  setTransport: (t) => set({ transport: t }),

  setConfig: (c) => set(state => {
    const names = new Map(state.peerNames);
    if (c.identity?.name && c.identity.name !== '(unnamed)') {
      names.set(c.identity.address, c.identity.name);
    }
    return { config: c, peerNames: names };
  }),

  setStatus: (s) => set({ status: s }),

  setAirtime: (a) => set({ airtime: a }),

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
      const isBroadcast = msg.to === 0xffffffff;
      const convId =
        msg.channelIndex !== undefined
          ? `ch:${msg.channelIndex}`
          : isBroadcast
          ? 'broadcast'
          : `dm:${msg.direction === 'outgoing' ? msg.to : msg.from}`;

      // Update conversation summary
      const convs = new Map(state.conversations);
      const prev = convs.get(convId);
      convs.set(convId, {
        id: convId,
        label: formatAddr(convId, state.peerNames),
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
          (msg.direction === 'incoming' ? 1 : 0),
      });

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
  setShowRoutes: (show: boolean) => set({ showRoutes: show }),

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
      // Rebuild conversations from cached messages
      const convs = new Map(state.conversations);
      for (const msg of msgs) {
        const isBroadcast = msg.to === 0xffffffff;
        const convId =
          msg.channelIndex !== undefined
            ? `ch:${msg.channelIndex}`
            : isBroadcast
            ? 'broadcast'
            : `dm:${msg.direction === 'outgoing' ? msg.to : msg.from}`;
        const prev = convs.get(convId);
        const shouldUpdate = !prev || !prev.lastMessageTime || msg.timestampMs > prev.lastMessageTime;
        if (shouldUpdate) {
          convs.set(convId, {
            id: convId,
            label: formatAddr(convId, state.peerNames),
            peerAddr:
              msg.channelIndex !== undefined || isBroadcast
                ? undefined
                : msg.direction === 'outgoing'
                ? msg.to
                : msg.from,
            channelIndex: msg.channelIndex,
            lastMessage: msg.text.slice(0, 60),
            lastMessageTime: msg.timestampMs,
            unreadCount: prev?.unreadCount ?? 0,
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
      return { activeConversationId: id, conversations: convs };
    }),
}));
