import type { SavedDevice } from '../lib/deviceBook';

// ─── Identity ──────────────────────────────────────────────────────────

export interface NodeIdentity {
  address: number;          // 32-bit node address
  pubkeyHash: number;       // 32-bit hash of public key
  name: string;             // Short name, max 32 chars
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

export interface BroadcastDeliveryRecipient {
  addr: number;
  status: 'delivered' | 'pending' | 'failed';
  hopCount: number;
  deliveredAtMs: number;
}

export interface Message {
  id: string;               // UUID (client-generated for outgoing, server msg_id for incoming)
  packetId?: string | number; // firmware packet_id, set on 'sent' status
  broadcastId?: string;     // correlation id for broadcast delivery telemetry
  broadcastRecipients?: BroadcastDeliveryRecipient[];
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
  name: 'critical' | 'normal' | 'broadcast' | 'receipt';
  remainingMs: number;
  maxMs: number;
  usedPct: number;          // 0-100
  refillAtMs: number;       // epoch ms
}

export interface AirtimeStatus {
  tiers: AirtimeTier[]; // critical, normal, broadcast, receipt
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

export type LocationSource = 'gps' | 'manual' | 'hybrid';

export interface LocationContactRule {
  address: string;          // 8-char uppercase hex node address
  enabled: boolean;
  tier: LocationTier;
  interval_s: number;
}

export interface LocationChannelTarget {
  channel: number;
  enabled: boolean;
  tier: LocationTier;
  interval_s: number;
}

/**
 * Per-peer SAS + verification state, from bramble.getPeerVerification (DM
 * forward-secrecy + SAS, Task 9). sas is a 7-digit string, empty when there
 * is no pin yet (peer never DM'd, not an error). keyChanged is a RAM-only
 * warning: the peer's identity key changed since the last verify.
 */
export interface PeerVerification {
  sas: string;
  verified: boolean;
  keyChanged: boolean;
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
  /** Current/default policy tier used for periodic sharing */
  tier: LocationTier;
  default_tier: LocationTier;
  /** Periodic sharing interval in seconds */
  interval_s: number;
  source: LocationSource;
  lat?: number;
  lon?: number;
  contact_rules: LocationContactRule[];
  channel_targets: LocationChannelTarget[];

  // Legacy fields used by older UI paths.
  contacts?: LocationContact[];
  defaultIntervalSec?: number;
  defaultDistanceTriggerM?: number;
  stationaryBackoff?: number;
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

export interface SendMessageResult {
  packetId: string;
  status: 'sent';
  fragmented: boolean;
  fragments_total?: number;  // only present if fragmented=true
  max_bytes: number;
  actual_bytes: number;
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

export type RuntimeMode = 'hosted' | 'local';

export interface ConnectionCapabilities {
  mode: RuntimeMode;
  localLanAllowed: boolean;
  localLanReason?: string;
}

export interface AppState {
  connectionState: ConnectionState;
  connectionError?: string;
  manualDisconnect: boolean;
  transport: Transport | null;
  connectionCapabilities: ConnectionCapabilities;
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
  devices: SavedDevice[];
  peerLocations: PeerLocation[];
  mapFocusAddr: number | null;
  peerVerifications: Map<number, PeerVerification>;
  trafficDebugStatus: TrafficDebugStatus | null;
  trafficEvents: TrafficEvent[];
  networkKeyStatus: NetworkKeyStatus | null;
  anchorStatus: AnchorStatus | null;
}

/**
 * Provisioning status of the control-plane network key. An unprovisioned node
 * has no usable key and is INERT (not meshing) until one is set; the webapp
 * surfaces this as a prominent top-level banner. The fingerprint is a one-way
 * SHA256(key)[0:4] (8 hex chars) that matches across nodes sharing a key, so an
 * operator can confirm the fleet converged; it is "00000000" when unprovisioned.
 */
export interface NetworkKeyStatus {
  provisioned: boolean;
  fingerprint: string;
}

/**
 * Trust-anchor provisioning status of a node, from bramble.getAnchorStatus.
 * `anchored` is whether the node pins to a fleet anchor at all; the fingerprint
 * (SHA256(anchor_pub)[0:4], 8 hex) is present only when anchored so an operator
 * can confirm the whole fleet points at the same anchor. `endorsed` reports
 * whether THIS node holds a cert that verifies against the CURRENT anchor.
 */
export interface AnchorStatus {
  anchored: boolean;
  anchor_fingerprint?: string;
  endorsed: boolean;
}

/**
 * A node's identity as returned raw by bramble.getIdentity, used by the anchor
 * enrollment flow. Distinct from NodeIdentity (the normalized display shape):
 * these are the on-the-wire hex fields, and ed25519_pub (64 hex) is the key the
 * operator endorses. address / pubkey_hash are hex strings.
 */
export interface NodeIdentityWire {
  address: string;
  pubkey_hash: string;
  ed25519_pub: string;
}
