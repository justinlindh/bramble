// ─── Identity ──────────────────────────────────────────────────────────

export interface NodeIdentity {
  address: number;          // 32-bit node address
  pubkeyHash: number;       // 32-bit hash of public key
  name: string;             // Short name, max 8 chars
  pubkeyB64: string;        // Base64-encoded public key (display only)
}

// ─── Radio ─────────────────────────────────────────────────────────────

export interface RadioConfig {
  txPowerDbm: number;       // 2-20
  sf: 7 | 8 | 9 | 10 | 11 | 12;
  bwKhz: 125 | 250 | 500;
  cr: 5 | 6 | 7 | 8;       // coding rate denominator (4/5 = 5, etc.)
  freqMhz: number;          // e.g. 915.0
}

// ─── Channels ──────────────────────────────────────────────────────────

export interface Channel {
  index: number;
  name: string;
  hasPsk: boolean;          // don't send PSK over the wire back to app
  epoch: number;            // key rotation epoch
  isDefault: boolean;
}

// ─── Neighbors & Routes ────────────────────────────────────────────────

export interface Neighbor {
  addr: number;
  rssi: number;
  snr: number;
  deliveryRate: number;     // 0-255, 255 = 100%
  lastHeardMs: number;      // milliseconds ago
  airtimeRemaining: number; // 0-100 %
}

export interface Route {
  dest: number;
  nextHop: number;
  hopCount: number;
  metric: number;
  state: 'active' | 'stale' | 'broken' | 'discovering';
  lastUsedMs: number;
}

// ─── Messages ──────────────────────────────────────────────────────────

export type MessageTier = 'broadcast' | 'normal' | 'critical';
export type MessageDirection = 'outgoing' | 'incoming';

export type DeliveryStatus =
  | 'queued'      // in app, not yet sent to node
  | 'sending'     // RPC call in flight
  | 'sent'        // node accepted (packet_id returned)
  | 'delivered'   // delivery receipt received (ACK from dest)
  | 'failed'      // all retries exhausted
  | 'timeout';    // no receipt within UI timeout

export interface RelayHop {
  addr: number;
  rssi: number;
}

export interface Message {
  id: string;               // UUID (client-generated for outgoing, server msg_id for incoming)
  packetId?: number;        // firmware packet_id, set on 'sent' status
  direction: MessageDirection;
  from: number;             // node address (0 = self)
  to: number;               // destination addr, 0xFFFFFFFF = broadcast
  channelIndex?: number;    // set for channel messages, undefined for DM
  text: string;
  timestampMs: number;      // client local time (outgoing) or decoded from packet (incoming)
  tier: MessageTier;
  status: DeliveryStatus;
  relayPath?: RelayHop[];   // populated from delivery receipt for Critical messages
}

// ─── Conversations ─────────────────────────────────────────────────────

export interface Conversation {
  /** 'dm:0x{addr}' or 'ch:{index}' */
  id: string;
  label: string;
  peerAddr?: number;        // set for DMs
  channelIndex?: number;    // set for channel convos
  lastMessage?: string;
  lastMessageTime?: number;
  unreadCount: number;
}

// ─── Airtime ───────────────────────────────────────────────────────────

export interface AirtimeTier {
  name: 'critical' | 'normal' | 'broadcast';
  remainingMs: number;
  maxMs: number;
  usedPct: number;          // 0-100
  refillAtMs: number;       // epoch ms
}

export interface AirtimeStatus {
  tiers: [AirtimeTier, AirtimeTier, AirtimeTier]; // critical, normal, broadcast
}

// ─── Adaptive Airtime Policy ────────────────────────────────────────────

export type AirtimePolicyMode = 'disabled' | 'stable' | 'dense' | 'churn';

export interface AirtimePolicyConfig {
  enabled: boolean;
  baseIntervalMs: number;       // baseline beacon interval (stable mode)
  minIntervalMs: number;        // minimum beacon interval (churn mode)
  maxIntervalMs: number;        // maximum beacon interval (dense mode)
  denseThreshold: number;       // neighbor count for dense mode
  churnWindowSec: number;       // time window for churn detection
  churnThreshold: number;       // join/leave events to trigger churn
  cooldownSec: number;          // hysteresis between mode transitions
}

export interface AirtimePolicyStatus {
  mode: AirtimePolicyMode;
  currentIntervalMs: number;
  neighborCount: number;
  neighborDelta: number;
  churnEvents: number;
  lastTransitionMs: number;
  congestionScore: number;      // 0-100, future feature
}

export interface AirtimePolicy {
  config: AirtimePolicyConfig;
  status: AirtimePolicyStatus;
}

// ─── Status ────────────────────────────────────────────────────────────

export interface NodeStatus {
  uptimeSec: number;
  freeHeapBytes: number;
  fwVersion: string;
  txCount: number;
  rxCount: number;
  droppedCount: number;
  neighborCount: number;
  routeCount: number;
  airtimeUsedMs: number;    // total since boot
  position?: Position;      // current GPS position if available
  gpsAvailable?: boolean;   // hardware has GPS module
  batteryMv?: number;       // battery voltage in millivolts
  batteryPct?: number;      // battery percentage 0-100
}

// ─── Location ──────────────────────────────────────────────────────────

export interface Position {
  lat: number;              // degrees
  lon: number;              // degrees
  alt: number;              // meters
  accuracy: number;         // meters
  speed: number;            // km/h
  heading: number;          // degrees 0-359
  timestampMs: number;      // epoch ms
}

export type LocationTier = 'off' | 'full' | 'coarse' | 'presence';

export interface LocationContact {
  addr: number;             // peer node address
  tier: LocationTier;
  intervalSec: number;      // update interval seconds
  distanceTriggerM: number; // distance trigger meters
}

export interface PeerLocation {
  addr: number;
  name: string;
  tier: LocationTier;
  position: Position | null;    // null for presence-only
  gridSquare?: string;          // set for coarse tier
  online: boolean;
  lastUpdatedMs: number;
}

export interface LocationConfig {
  enabled: boolean;
  contacts: LocationContact[];
  defaultIntervalSec: number;
  defaultDistanceTriggerM: number;
  stationaryBackoff: number;
}

// ─── Config (full) ─────────────────────────────────────────────────────

export interface BrambleConfig {
  identity: NodeIdentity;
  radio: RadioConfig;
  channels: Channel[];
  mailboxEnabled: boolean;
  location: LocationConfig;
}

// ─── RPC types ─────────────────────────────────────────────────────────

export interface SendParams {
  dest: number;             // 0xFFFFFFFF for broadcast, 0xFFFFFFFE for default channel
  text: string;
  tier?: MessageTier;
  channelIndex?: number;    // set for channel messages
}

export interface IncomingMessage {
  from: number;
  to: number;
  text: string;
  tier: MessageTier;
  channelIndex?: number;
  timestamp: number;        // node epoch seconds
  msgId: string;
}

export interface AckNotification {
  packetId: number;
  status: 'delivered' | 'failed';
  relayPath?: RelayHop[];
}

// ─── Probe / Network Reach ─────────────────────────────────────────────

export interface ProbeResponse {
  responderAddr: number;
  hopCount: number;
  rssi: number;
  snr: number;
  pathLen: number;
  relayPath?: number[];
  latencyMs?: number;
  receivedAt?: number;
  seenRounds?: number;
  confidence?: number;
}

export interface ProbeResult {
  probeId: number;
  sentAt: number;
  ackWindow: number;
  responses: ProbeResponse[];
  complete: boolean;
}

// ─── Transport abstraction ─────────────────────────────────────────────

export interface Transport {
  readonly connected: boolean;
  connect(): Promise<void>;
  disconnect(): Promise<void>;
  sendRPC<T = unknown>(method: string, params?: Record<string, unknown>, timeoutMs?: number): Promise<T>;
  onNotification(cb: (method: string, params: unknown) => void): void;
}

export type TransportType = 'serial' | 'ble' | 'websocket' | 'wifi';

// ─── App state ─────────────────────────────────────────────────────────

export type ConnectionState = 'disconnected' | 'connecting' | 'connected' | 'error';

// ─── Traffic Debug ─────────────────────────────────────────────────────

export type TrafficCategory = 
  | 'beacon'
  | 'timesync'
  | 'routing'
  | 'ack'
  | 'chat'
  | 'maintenance'
  | 'other';

export type TrafficDirection = 'tx' | 'rx';

export type AirtimeBucket = 'broadcast' | 'normal' | 'critical';

export interface TrafficEvent {
  seq: number;
  timestampMs: number;
  direction: TrafficDirection;
  category: TrafficCategory;
  packetType: string;
  tier: MessageTier;
  airtimeBucket: AirtimeBucket;
  airtimeDebitUs: number;
  queueDepth?: number;
  rssi?: number;
  snr?: number;
}

export interface TrafficDebugConfig {
  enabled: boolean;
  includeTx: boolean;
  includeRx: boolean;
  sampleRate: number;  // 0-100%
}

export interface TrafficDebugStatus {
  config: TrafficDebugConfig;
  ringSize: number;
  ringUsed: number;
  droppedCount: number;
  lastSeq: number;
}

export interface AppState {
  connectionState: ConnectionState;
  connectionError?: string;
  transport: Transport | null;
  config: BrambleConfig | null;
  status: NodeStatus | null;
  airtime: AirtimeStatus | null;
  airtimePolicy: AirtimePolicy | null;
  neighbors: Neighbor[];
  routes: Route[];
  messages: Message[];
  conversations: Map<string, Conversation>;
  activeConversationId: string;
  activeTab: string;
  probeResult: ProbeResult | null;
  probeCollecting: boolean;
  peerLocations: PeerLocation[];
  mapFocusAddr: number | null;
  trafficDebugStatus: TrafficDebugStatus | null;
  trafficEvents: TrafficEvent[];
}
