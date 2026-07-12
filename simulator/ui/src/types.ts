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
  kind?: string;    // "firmware" for emulated pager nodes (device view)
}

// Per-firmware-node device state feeding the pager device view. Populated from
// the broker's "device_fb" / "device_ind" / "console" events (see gosim
// extnode.go handleFB/handleInd/emitConsole).
export interface DeviceState {
  node: string;      // hello id (matches SimNode.id)
  addr?: string;     // "0x........"
  fb: string | null; // latest base64 1bpp framebuffer
  fbKind: 'partial' | 'full';
  fbBusyMs: number;  // engine-reported panel busy window
  fbSeq: number;     // increments per received frame (drives the Epaper)
  led: boolean;      // notification LED
  buzzerHz: number;  // 0 = silent
  vibra: boolean;    // motor on
  vibraSeq: number;  // increments on each vibra pulse (drives the shake)
  console: string[]; // rolling firmware console lines
}

// Link between two nodes (for future use)
export interface SimLink {
  from: string;
  to: string;
  quality: number; // 0-1
}

// Per-link RSSI/SNR quality tracking (rolling average)
export interface LinkQuality {
  key: string;           // "nodeA-nodeB" sorted
  rssi: number;          // rolling average RSSI in dBm (e.g. -75)
  snr: number;           // rolling average SNR in dB (e.g. 45)
  sampleCount: number;
  lastUpdatedAt: number; // Date.now() ms
}

// Per-neighbor RSSI info for NodeHealthCard display
export interface NeighborRSSI {
  nodeId: string;
  rssi: number;
  snr: number;
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
  // Enhanced metrics from component integration
  retried?: number;
  deliveredOnRetry?: number;
  dedupDropped?: number;
  airtimeDeferred?: number;
  fragmentsSent?: number;
  fragmentsReassembled?: number;
  cryptoEncrypted?: number;
  cryptoDecrypted?: number;
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

// Delivery path animation (green trace)
export interface DeliveryPathAnimation {
  id: number;
  path: string[];      // node ids in order
  createdAt: number;   // Date.now() ms
  durationMs: number;  // total duration for the trace
}

// Per-node statistics
export interface NodeStats {
  packetsSent: number;
  packetsReceived: number;
  packetsForwarded: number;
  routeCount: number;
  messagesOriginated: number;
  messagesDelivered: number;
}

// Delivery record for path history
export interface DeliveryRecord {
  id: number;
  timestamp_us: number;
  from: string;    // source addr/node
  to: string;      // dest addr/node
  path: string[];  // node ids along the path
  hopCount: number;
  latencyMs?: number;
}

// Link activity tracking
export interface LinkActivity {
  key: string;       // "nodeA-nodeB" sorted
  packetCount: number;
  lastActiveAt: number; // Date.now() ms
}

// Broken link tracking
export interface BrokenLink {
  key: string;       // "nodeA-nodeB" sorted
  brokenAt: number;  // Date.now() ms
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
  // New state for network health visualization
  nodeStats: Map<string, NodeStats>;
  deliveryPaths: DeliveryPathAnimation[];
  deliveryPathCounter: number;
  deliveryRecords: DeliveryRecord[];
  deliveryRecordCounter: number;
  linkActivity: Map<string, LinkActivity>;
  brokenLinks: Map<string, BrokenLink>;
  selectedNodeId: string | null;
  // RSSI/SNR per-link quality tracking
  linkQuality: Map<string, LinkQuality>;
  // Per-firmware-node device state for the pager device view, keyed by the
  // node's emu-link hello id (same id the mesh uses).
  devices: Map<string, DeviceState>;
  // Firmware hello ids in attach order. The gosim supervisor spawns firmware
  // instances strictly in declaration order (waitAttach per instance), so the
  // i-th firmware join is process label "<label>-i"; this lets the UI route the
  // supervisor's stdout console (tagged with the process label) to the device
  // keyed by its hello id (tagged on fb/ind). See gosim supervisor.go /
  // extnode.go.
  firmwareOrder: string[];
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
  | { type: 'PLAYBACK_STARTED' }
  | { type: 'RESET' }
  | { type: 'ADD_DELIVERY_PATH'; path: string[]; from: string; to: string; timestamp_us: number; latencyMs?: number }
  | { type: 'EXPIRE_DELIVERY_PATHS'; now: number }
  | { type: 'TRACK_PACKET_SENT'; node: string; dest: string }
  | { type: 'TRACK_PACKET_RECEIVED'; node: string; from: string }
  | { type: 'TRACK_ROUTE_ADDED'; node: string }
  | { type: 'TRACK_LINK_BROKEN'; from: string; to: string }
  | { type: 'TRACK_LINK_ACTIVITY'; from: string; to: string }
  | { type: 'TRACK_MESSAGE_SENT'; from: string }
  | { type: 'TRACK_MESSAGE_DELIVERED'; to: string }
  | { type: 'SELECT_NODE'; nodeId: string | null }
  | { type: 'EXPIRE_BROKEN_LINKS'; now: number }
  | { type: 'TRACK_LINK_RSSI'; from: string; to: string; rssi: number; snr: number }
  | { type: 'DEVICE_FB'; node: string; addr?: string; kind: 'partial' | 'full'; fb: string; busyMs: number }
  | { type: 'DEVICE_IND'; node: string; addr?: string; led: boolean; buzzerHz: number; vibra: boolean }
  | { type: 'DEVICE_CONSOLE'; node: string; line: string };
