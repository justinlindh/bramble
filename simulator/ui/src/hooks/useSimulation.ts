import { useEffect, useReducer, useRef, useCallback } from 'react';
import type {
  SimState, SimAction, SimNode, Metrics, RawSimEvent, PacketAnimation,
  NodeStats, DeliveryPathAnimation, DeliveryRecord, BrokenLink,
} from '../types';

const MAX_EVENTS = 100;
const MAX_DELIVERY_RECORDS = 200;
const PACKET_ANIM_DURATION_MS = 500;
const DELIVERY_PATH_DURATION_MS = 3000;
const BROKEN_LINK_FADE_MS = 10000;

function makeLinkKey(a: string, b: string): string {
  return a < b ? `${a}-${b}` : `${b}-${a}`;
}

function getOrCreateNodeStats(map: Map<string, NodeStats>, nodeId: string): NodeStats {
  let s = map.get(nodeId);
  if (!s) {
    s = { packetsSent: 0, packetsReceived: 0, packetsForwarded: 0, routeCount: 0, messagesOriginated: 0, messagesDelivered: 0 };
    map.set(nodeId, s);
  }
  return s;
}

const initialState: SimState = {
  connected: false,
  running: false,
  ready: false,
  currentTime: 0,
  nodes: new Map(),
  links: [],
  metrics: null,
  events: [],
  eventCounter: 0,
  recentPackets: [],
  packetCounter: 0,
  nodeStats: new Map(),
  deliveryPaths: [],
  deliveryPathCounter: 0,
  deliveryRecords: [],
  deliveryRecordCounter: 0,
  linkActivity: new Map(),
  brokenLinks: new Map(),
  selectedNodeId: null,
};

function simReducer(state: SimState, action: SimAction): SimState {
  switch (action.type) {
    case 'CONNECTED':
      return { ...state, connected: true, running: true };

    case 'DISCONNECTED':
      return { ...state, connected: false, running: false };

    case 'SIM_ENDED':
      return { ...state, running: false, ready: false };

    case 'SIM_READY':
      return { ...state, running: false, ready: true };

    case 'PLAYBACK_STARTED':
      return state.running ? state : { ...state, running: true, ready: false };

    case 'RESET':
      return { ...initialState, connected: true };

    case 'ADD_NODE': {
      const nodes = new Map(state.nodes);
      nodes.set(action.node.id, action.node);
      return { ...state, nodes, currentTime: Math.max(state.currentTime, action.node.lastSeen) };
    }

    case 'UPDATE_NODE': {
      const nodes = new Map(state.nodes);
      const existing = nodes.get(action.id);
      if (existing) {
        nodes.set(action.id, { ...existing, x: action.x, y: action.y, active: true, lastSeen: action.timestamp_us });
      } else {
        nodes.set(action.id, { id: action.id, x: action.x, y: action.y, active: true, lastSeen: action.timestamp_us });
      }
      return { ...state, nodes, currentTime: Math.max(state.currentTime, action.timestamp_us) };
    }

    case 'REMOVE_NODE': {
      const nodes = new Map(state.nodes);
      const existing = nodes.get(action.id);
      if (existing) {
        nodes.set(action.id, { ...existing, active: false, lastSeen: action.timestamp_us });
      }
      return { ...state, nodes, currentTime: Math.max(state.currentTime, action.timestamp_us) };
    }

    case 'UPDATE_METRICS':
      return {
        ...state,
        metrics: action.metrics,
        currentTime: Math.max(state.currentTime, action.metrics.timestamp_us),
      };

    case 'ADD_EVENT': {
      const id = state.eventCounter + 1;
      const newEvent = { ...action.event, id };
      const events = [...state.events, newEvent].slice(-MAX_EVENTS);
      return {
        ...state,
        events,
        eventCounter: id,
        currentTime: Math.max(state.currentTime, action.event.timestamp_us),
      };
    }

    case 'ADD_PACKET_ANIM': {
      const id = state.packetCounter + 1;
      const anim: PacketAnimation = {
        id,
        from: action.from,
        to: action.to,
        pkt_type: action.pkt_type,
        createdAt: Date.now(),
        durationMs: PACKET_ANIM_DURATION_MS,
      };
      return {
        ...state,
        packetCounter: id,
        recentPackets: [...state.recentPackets, anim],
      };
    }

    case 'EXPIRE_PACKETS': {
      const alive = state.recentPackets.filter(
        p => action.now - p.createdAt < p.durationMs
      );
      return { ...state, recentPackets: alive };
    }

    case 'ADD_DELIVERY_PATH': {
      const id = state.deliveryPathCounter + 1;
      const pathAnim: DeliveryPathAnimation = {
        id,
        path: action.path,
        createdAt: Date.now(),
        durationMs: DELIVERY_PATH_DURATION_MS,
      };
      const recId = state.deliveryRecordCounter + 1;
      const record: DeliveryRecord = {
        id: recId,
        timestamp_us: action.timestamp_us,
        from: action.from,
        to: action.to,
        path: action.path,
        hopCount: action.path.length - 1,
        latencyMs: action.latencyMs,
      };
      return {
        ...state,
        deliveryPathCounter: id,
        deliveryPaths: [...state.deliveryPaths, pathAnim],
        deliveryRecordCounter: recId,
        deliveryRecords: [...state.deliveryRecords, record].slice(-MAX_DELIVERY_RECORDS),
      };
    }

    case 'EXPIRE_DELIVERY_PATHS': {
      const alive = state.deliveryPaths.filter(
        p => action.now - p.createdAt < p.durationMs
      );
      if (alive.length === state.deliveryPaths.length) return state;
      return { ...state, deliveryPaths: alive };
    }

    case 'TRACK_PACKET_SENT': {
      const nodeStats = new Map(state.nodeStats);
      const s = { ...getOrCreateNodeStats(nodeStats, action.node) };
      s.packetsSent++;
      nodeStats.set(action.node, s);
      // Track link activity
      const linkActivity = new Map(state.linkActivity);
      const key = makeLinkKey(action.node, action.dest);
      const la = linkActivity.get(key);
      linkActivity.set(key, {
        key,
        packetCount: (la?.packetCount ?? 0) + 1,
        lastActiveAt: Date.now(),
      });
      return { ...state, nodeStats, linkActivity };
    }

    case 'TRACK_PACKET_RECEIVED': {
      const nodeStats = new Map(state.nodeStats);
      const s = { ...getOrCreateNodeStats(nodeStats, action.node) };
      s.packetsReceived++;
      nodeStats.set(action.node, s);
      const linkActivity = new Map(state.linkActivity);
      const key = makeLinkKey(action.node, action.from);
      const la = linkActivity.get(key);
      linkActivity.set(key, {
        key,
        packetCount: (la?.packetCount ?? 0) + 1,
        lastActiveAt: Date.now(),
      });
      return { ...state, nodeStats, linkActivity };
    }

    case 'TRACK_ROUTE_ADDED': {
      const nodeStats = new Map(state.nodeStats);
      const s = { ...getOrCreateNodeStats(nodeStats, action.node) };
      s.routeCount++;
      nodeStats.set(action.node, s);
      return { ...state, nodeStats };
    }

    case 'TRACK_LINK_BROKEN': {
      const brokenLinks = new Map(state.brokenLinks);
      const key = makeLinkKey(action.from, action.to);
      const bl: BrokenLink = { key, brokenAt: Date.now() };
      brokenLinks.set(key, bl);
      return { ...state, brokenLinks };
    }

    case 'TRACK_LINK_ACTIVITY': {
      const linkActivity = new Map(state.linkActivity);
      const key = makeLinkKey(action.from, action.to);
      const la = linkActivity.get(key);
      linkActivity.set(key, {
        key,
        packetCount: (la?.packetCount ?? 0) + 1,
        lastActiveAt: Date.now(),
      });
      return { ...state, linkActivity };
    }

    case 'TRACK_MESSAGE_SENT': {
      const nodeStats = new Map(state.nodeStats);
      const s = { ...getOrCreateNodeStats(nodeStats, action.from) };
      s.messagesOriginated++;
      nodeStats.set(action.from, s);
      return { ...state, nodeStats };
    }

    case 'TRACK_MESSAGE_DELIVERED': {
      const nodeStats = new Map(state.nodeStats);
      const s = { ...getOrCreateNodeStats(nodeStats, action.to) };
      s.messagesDelivered++;
      nodeStats.set(action.to, s);
      return { ...state, nodeStats };
    }

    case 'SELECT_NODE':
      return { ...state, selectedNodeId: action.nodeId };

    case 'EXPIRE_BROKEN_LINKS': {
      const alive = new Map<string, BrokenLink>();
      for (const [k, v] of state.brokenLinks) {
        if (action.now - v.brokenAt < BROKEN_LINK_FADE_MS) {
          alive.set(k, v);
        }
      }
      if (alive.size === state.brokenLinks.size) return state;
      return { ...state, brokenLinks: alive };
    }

    default:
      return state;
  }
}

// Resolve addr to node id
function resolveAddrToId(addr: string, nodes: Map<string, SimNode>): string | null {
  if (nodes.has(addr)) return addr;
  for (const node of nodes.values()) {
    if (node.addr === addr) return node.id;
  }
  return null;
}

function parseEvent(raw: RawSimEvent, nodes: Map<string, SimNode>): SimAction[] {
  const actions: SimAction[] = [];

  const { type, timestamp_us: rawTs, ...rest } = raw;
  const timestamp_us = typeof rawTs === 'number' ? rawTs : 0;
  actions.push({
    type: 'ADD_EVENT',
    event: { type, timestamp_us, details: rest },
  });

  const setupTypes = new Set(['sim_reset', 'sim_ready', 'node_joined', 'config']);
  if (timestamp_us > 0 && !setupTypes.has(type)) {
    actions.push({ type: 'PLAYBACK_STARTED' });
  }

  switch (type) {
    case 'sim_reset': {
      actions.unshift({ type: 'RESET' });
      break;
    }
    case 'sim_ready': {
      actions.push({ type: 'SIM_READY' });
      break;
    }
    case 'node_joined': {
      const node: SimNode = {
        id: raw.node as string,
        addr: raw.addr as string | undefined,
        x: (raw.x as number) ?? 0,
        y: (raw.y as number) ?? 0,
        active: true,
        lastSeen: timestamp_us,
      };
      actions.push({ type: 'ADD_NODE', node });
      break;
    }
    case 'node_moved': {
      actions.push({
        type: 'UPDATE_NODE',
        id: raw.node as string,
        x: raw.x as number,
        y: raw.y as number,
        timestamp_us,
      });
      break;
    }
    case 'node_left': {
      actions.push({ type: 'REMOVE_NODE', id: raw.node as string, timestamp_us });
      break;
    }
    case 'metrics': {
      const totalPackets = (raw.total_packets as number) ?? 0;
      const messagesSent = (raw.messages_sent as number) ?? 0;
      const delivered = (raw.delivered as number) ?? 0;
      const dropped = (raw.dropped as number) ?? 0;
      const metrics: Metrics = {
        timestamp_us,
        activeNodes: (raw.active_nodes as number) ?? 0,
        totalPackets,
        messagesSent,
        delivered,
        dropped,
        avgLatencyMs: (raw.avg_latency_ms as number) ?? 0,
        deliveryRate: messagesSent > 0 ? (delivered / messagesSent) * 100 : 0,
        retried: raw.retried as number | undefined,
        deliveredOnRetry: raw.delivered_on_retry as number | undefined,
        dedupDropped: raw.dedup_dropped as number | undefined,
        airtimeDeferred: raw.airtime_deferred as number | undefined,
        fragmentsSent: raw.fragments_sent as number | undefined,
        fragmentsReassembled: raw.fragments_reassembled as number | undefined,
        cryptoEncrypted: raw.crypto_encrypted as number | undefined,
        cryptoDecrypted: raw.crypto_decrypted as number | undefined,
      };
      actions.push({ type: 'UPDATE_METRICS', metrics });
      break;
    }
    case 'sim_ended': {
      actions.push({ type: 'SIM_ENDED' });
      break;
    }
    case 'final_metrics': {
      // Treat same as metrics
      const totalPackets = (raw.total_packets as number) ?? 0;
      const messagesSent = (raw.messages_sent as number) ?? 0;
      const delivered = (raw.delivered as number) ?? 0;
      const dropped = (raw.dropped as number) ?? 0;
      const metrics: Metrics = {
        timestamp_us,
        activeNodes: (raw.active_nodes as number) ?? 0,
        totalPackets,
        messagesSent,
        delivered,
        dropped,
        avgLatencyMs: (raw.avg_latency_ms as number) ?? 0,
        deliveryRate: messagesSent > 0 ? (delivered / messagesSent) * 100 : 0,
        retried: raw.retried as number | undefined,
        deliveredOnRetry: raw.delivered_on_retry as number | undefined,
        dedupDropped: raw.dedup_dropped as number | undefined,
        airtimeDeferred: raw.airtime_deferred as number | undefined,
        fragmentsSent: raw.fragments_sent as number | undefined,
        fragmentsReassembled: raw.fragments_reassembled as number | undefined,
        cryptoEncrypted: raw.crypto_encrypted as number | undefined,
        cryptoDecrypted: raw.crypto_decrypted as number | undefined,
      };
      actions.push({ type: 'UPDATE_METRICS', metrics });
      break;
    }
    case 'packet_sent': {
      const fromNode = raw.node as string | undefined;
      const destAddr = (raw.dest ?? raw.dest_addr) as string | undefined;
      const pktType  = (raw.pkt_type as string) ?? 'DATA';
      if (fromNode && destAddr) {
        actions.push({
          type: 'ADD_PACKET_ANIM',
          from: fromNode,
          to: destAddr,
          pkt_type: pktType,
        });
        actions.push({ type: 'TRACK_PACKET_SENT', node: fromNode, dest: destAddr });
        // Track link activity using resolved node id
        const destId = resolveAddrToId(destAddr, nodes);
        if (destId) {
          actions.push({ type: 'TRACK_LINK_ACTIVITY', from: fromNode, to: destId });
        }
      }
      break;
    }
    case 'packet_received': {
      const node = raw.node as string | undefined;
      const fromAddr = (raw.from ?? raw.from_addr) as string | undefined;
      if (node && fromAddr) {
        actions.push({ type: 'TRACK_PACKET_RECEIVED', node, from: fromAddr });
        const fromId = resolveAddrToId(fromAddr, nodes);
        if (fromId) {
          actions.push({ type: 'TRACK_LINK_ACTIVITY', from: node, to: fromId });
        }
      }
      break;
    }
    case 'message_sent': {
      const fromNode = (raw.node ?? raw.from) as string | undefined;
      if (fromNode) {
        actions.push({ type: 'TRACK_MESSAGE_SENT', from: fromNode });
      }
      break;
    }
    case 'message_delivered': {
      const path = raw.path as string[] | undefined;
      const fromAddr = raw.from as string | undefined;
      const toNode = (raw.node ?? raw.to) as string | undefined;
      const latencyMs = raw.latency_ms as number | undefined;
      if (toNode) {
        actions.push({ type: 'TRACK_MESSAGE_DELIVERED', to: toNode });
      }
      if (path && path.length >= 2) {
        // Resolve addresses in path to node IDs
        const resolvedPath = path.map(addr => resolveAddrToId(addr, nodes) ?? addr);
        actions.push({
          type: 'ADD_DELIVERY_PATH',
          path: resolvedPath,
          from: fromAddr ?? resolvedPath[0],
          to: toNode ?? resolvedPath[resolvedPath.length - 1],
          timestamp_us,
          latencyMs,
        });
      }
      break;
    }
    case 'route_added': {
      const node = raw.node as string | undefined;
      if (node) {
        actions.push({ type: 'TRACK_ROUTE_ADDED', node });
      }
      break;
    }
    case 'link_broken': {
      const from = raw.from as string | undefined;
      const to = raw.to as string | undefined;
      if (from && to) {
        actions.push({ type: 'TRACK_LINK_BROKEN', from, to });
      }
      break;
    }
  }

  return actions;
}

export function useSimulation() {
  const [state, dispatch] = useReducer(simReducer, initialState);
  const wsRef = useRef<WebSocket | null>(null);
  const stateRef = useRef(state);
  stateRef.current = state;

  const selectNode = useCallback((nodeId: string | null) => {
    dispatch({ type: 'SELECT_NODE', nodeId });
  }, []);

  // Periodically expire old animations
  useEffect(() => {
    const id = setInterval(() => {
      const now = Date.now();
      dispatch({ type: 'EXPIRE_PACKETS', now });
      dispatch({ type: 'EXPIRE_DELIVERY_PATHS', now });
      dispatch({ type: 'EXPIRE_BROKEN_LINKS', now });
    }, 100);
    return () => clearInterval(id);
  }, []);

  useEffect(() => {
    const wsUrl = `ws://${window.location.host}`;
    console.log(`[useSimulation] Connecting to ${wsUrl}`);

    let ws: WebSocket;
    let reconnectTimer: ReturnType<typeof setTimeout> | undefined;

    function connect() {
      ws = new WebSocket(wsUrl);
      wsRef.current = ws;

      ws.addEventListener('open', () => {
        console.log('[useSimulation] Connected');
        dispatch({ type: 'CONNECTED' });
      });

      ws.addEventListener('message', (event: MessageEvent<string>) => {
        try {
          const raw = JSON.parse(event.data) as RawSimEvent;
          const actions = parseEvent(raw, stateRef.current.nodes);
          for (const action of actions) {
            dispatch(action);
          }
        } catch (err) {
          console.warn('[useSimulation] Failed to parse message:', event.data, err);
        }
      });

      ws.addEventListener('close', () => {
        console.log('[useSimulation] Connection closed');
        dispatch({ type: 'DISCONNECTED' });
        wsRef.current = null;
      });

      ws.addEventListener('error', (err) => {
        console.error('[useSimulation] WebSocket error:', err);
      });
    }

    connect();

    return () => {
      clearTimeout(reconnectTimer);
      ws?.close();
      wsRef.current = null;
    };
  }, []);

  return { state, ws: wsRef, selectNode };
}
