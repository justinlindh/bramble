// Raw event shapes emitted by the C engine
export interface RawSimEvent {
  type: string;
  timestamp_us: number;
  [key: string]: unknown;
}

// Processed event for the log
export interface SimEvent {
  id: number;
  type: string;
  timestamp_us: number;
  details: Record<string, unknown>;
}

// Node state
export interface SimNode {
  id: string;
  addr?: string;    // hex address from engine, e.g. "0x01000000"
  x: number;
  y: number;
  active: boolean;
  lastSeen: number; // timestamp_us
}

// Link between two nodes (for future use)
export interface SimLink {
  from: string;
  to: string;
  quality: number; // 0-1
}

// Metrics snapshot
export interface Metrics {
  timestamp_us: number;
  activeNodes: number;
  totalPackets: number;
  messagesSent: number;
  delivered: number;
  dropped: number;
  avgLatencyMs: number;
  deliveryRate: number; // 0-100 (messages delivered / messages sent)
}

// Animated packet dot
export interface PacketAnimation {
  id: number;
  from: string;      // node id
  to: string;        // node id (or dest addr string for non-node targets)
  pkt_type: string;  // RREQ | RREP | RERR | DATA | BEACON
  createdAt: number; // Date.now() ms
  durationMs: number;
}

// Overall simulation state
export interface SimState {
  connected: boolean;
  running: boolean;
  ready: boolean;  // sim loaded and paused, waiting for play
  currentTime: number; // microseconds
  nodes: Map<string, SimNode>;
  links: SimLink[];
  metrics: Metrics | null;
  events: SimEvent[];
  eventCounter: number;
  recentPackets: PacketAnimation[];
  packetCounter: number;
}

// Actions for the reducer
export type SimAction =
  | { type: 'CONNECTED' }
  | { type: 'DISCONNECTED' }
  | { type: 'ADD_NODE'; node: SimNode }
  | { type: 'UPDATE_NODE'; id: string; x: number; y: number; timestamp_us: number }
  | { type: 'REMOVE_NODE'; id: string; timestamp_us: number }
  | { type: 'UPDATE_METRICS'; metrics: Metrics }
  | { type: 'ADD_EVENT'; event: Omit<SimEvent, 'id'> }
  | { type: 'ADD_PACKET_ANIM'; from: string; to: string; pkt_type: string }
  | { type: 'EXPIRE_PACKETS'; now: number }
  | { type: 'SIM_ENDED' }
  | { type: 'SIM_READY' }
  | { type: 'RESET' };
