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
  txPowerDbm: number;       // 2-22 (SX1262 high-power PA tops out at +22 dBm)
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
  /** 'broadcast', 'dm:{addr}' (decimal), or 'ch:{index}'; see parseConversationId in store/index.ts */
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
  gpsEnabled?: boolean;     // persisted GPS power preference (independent of gpsAvailable)
  // GNSS observability. All optional and never defaulted: an absent field means
  // the firmware predates them, which must render as unknown rather than as a
  // receiver hearing nothing.
  gpsState?: 'absent' | 'no_signal' | 'acquiring' | 'fix'; // three-way GNSS state
  gpsSatsInView?: number;   // satellites the receiver lists, tracked or predicted
  gpsSatsTracked?: number;  // satellites reporting a nonzero C/N0
  gpsSatsUsed?: number;     // satellites in the fix computation
  gpsSnrMaxDbHz?: number;   // best C/N0 in dB-Hz
  gpsFixQuality?: number;   // GGA fix quality digit
  batteryMv?: number;       // battery voltage in millivolts
  batteryPct?: number;      // battery percentage 0-100
  charging?: 'unknown' | 'no' | 'yes'; // hardware-informed charging state; "yes" includes plugged-in-and-full
  present?: boolean;        // whether battery-sensing hardware initialized; false means batteryMv/batteryPct are not meaningful
  hardware?: string;        // hardware profile (e.g. "heltec_v4")
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

/** A named zone the node offers in its on-device picker. */
export interface TimezonePreset {
  label: string;
  spec: string;
}

/** The zone the node renders its own clock in, as reported by getTimezone. */
export interface TimezoneInfo {
  timezone: string;
  defaultTimezone: string;
  configured: boolean;
  presets: TimezonePreset[];
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
}

// ─── Config (full) ─────────────────────────────────────────────────────

export interface BrambleConfig {
  identity: NodeIdentity;
  radio: RadioConfig;
  channels: Channel[];
  mailboxEnabled: boolean;
  location: LocationConfig;
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

// ─── Attested roll-call ────────────────────────────────────────────────

export interface RollCallResponder {
  addr: number;
  /** True when an Ed25519 signature verified against this address's pin. */
  responded: boolean;
  /** Milliseconds INTO the roll-call, not device uptime. */
  atMs?: number;
  round?: number;
  relayPath?: number[];
}

export interface RollCallLedger {
  /** False when this node has never started a roll-call. */
  active: boolean;
  /** True while the ledger is still collecting answers. */
  open: boolean;
  rollcallId?: string;
  text?: string;
  roundsSent: number;
  roundsTotal: number;
  windowMs: number;
  elapsedMs: number;
  minIntervalMs: number;
  maxTextBytes: number;
  /**
   * True when the expected set is anchor-certified and therefore
   * authoritative. False means the ledger reports observed responders only
   * and can name nobody missing: see docs/rollcall.md.
   */
  anchored: boolean;
  expected: number;
  responded: number;
  unattested: number;
  overflow: number;
  late: number;
  pendingDropped: number;
  /** Answers this node refused because its hourly answer budget was spent. */
  answerLimited: number;
  missing: number[];
  missingCount: number;
  responders: RollCallResponder[];
}

/**
 * Outcome of bramble.startRollCall. A refusal is ok:false with a reason and
 * the interval to wait, not a thrown error: the mesh is telling the operator
 * to come back later, which is not the same as the call failing.
 */
export interface RollCallStart {
  ok: boolean;
  rollcallId?: string;
  reason?: 'busy' | 'rate_limited' | 'not_transmitted';
  retryAfterMs?: number;
  expected: number;
  anchored: boolean;
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
  /**
   * True when connectionError is an authentication failure, classified from
   * the RAW error at the connect() boundary. The overlay highlights the
   * token field from this flag; regexing the friendly display text instead
   * forced every ERROR_MAP entry to avoid substrings like 'auth'.
   */
  connectionErrorIsAuth: boolean;
  /**
   * Which surface started the current connect attempt. Decides where the
   * overlay renders attempt feedback (errors, the pairing banner): next to
   * the saved-device rows or in the bottom slot under the form. Lives in the
   * store because the overlay can unmount mid-attempt (the identity guard
   * passes through 'connected' before settling), which would reset
   * component-local attribution.
   */
  attemptSource: 'row' | 'form';
  /**
   * True while the transport reports an OS pairing prompt is up during a BLE
   * connect. The overlay uses it to tell the user to type the code shown on
   * the node; without it a first-time pairing reads as a silent hang.
   */
  pairingPending: boolean;
  connectionCapabilities: ConnectionCapabilities;
  /**
   * False until the capabilities fetch resolves. Until then
   * connectionCapabilities holds the hosted defaults, which are a placeholder,
   * not a verdict: the UI must not state a restriction from them.
   */
  capabilitiesLoaded: boolean;
  config: BrambleConfig | null;
  status: NodeStatus | null;
  airtime: AirtimeStatus | null;
  // undefined = never fetched since connect (distinct from [] = fetched, none found)
  neighbors: Neighbor[] | undefined;
  routes: Route[];
  messages: Message[];
  conversations: Map<string, Conversation>;
  activeConversationId: string;
  activeTab: string;
  probeResult: ProbeResult | null;
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
