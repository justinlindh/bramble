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
  delivered: number;
  dropped: number;
  avgLatencyMs: number;
  deliveryRate: number; // 0-100
}

// Overall simulation state
export interface SimState {
  connected: boolean;
  running: boolean;
  currentTime: number; // microseconds
  nodes: Map<string, SimNode>;
  links: SimLink[];
  metrics: Metrics | null;
  events: SimEvent[];
  eventCounter: number;
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
  | { type: 'SIM_ENDED' };
