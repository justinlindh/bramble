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
} from '../types/bramble';

function formatAddr(id: string): string {
  if (id === 'broadcast') return '📢 Broadcast';
  if (id.startsWith('ch:')) return `#ch-${id.slice(3)}`;
  if (id.startsWith('dm:')) return `0x${Number(id.slice(3)).toString(16).toUpperCase()}`;
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
  setProbeResult: (r: ProbeResult | null) => void;
  setProbeCollecting: (c: boolean) => void;
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
  probeResult: null,
  probeCollecting: false,

  // ─── Actions ─────────────────────────────────────────────────────────
  setConnectionState: (s, err?) =>
    set({ connectionState: s, connectionError: err }),

  setTransport: (t) => set({ transport: t }),

  setConfig: (c) => set({ config: c }),

  setStatus: (s) => set({ status: s }),

  setAirtime: (a) => set({ airtime: a }),

  setNeighbors: (n) => set({ neighbors: n }),

  setRoutes: (r) => set({ routes: r }),

  addMessage: (msg: Message) =>
    set(state => {
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
        label: prev?.label ?? formatAddr(convId),
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

  setProbeResult: (r) => set({ probeResult: r }),
  setProbeCollecting: (c) => set({ probeCollecting: c }),

  setActiveConversation: (id: string) =>
    set(state => {
      const convs = new Map(state.conversations);
      const conv = convs.get(id);
      if (conv) convs.set(id, { ...conv, unreadCount: 0 });
      return { activeConversationId: id, conversations: convs };
    }),
}));
