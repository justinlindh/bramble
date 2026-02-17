import { useEffect, useReducer, useRef } from 'react';
import type { SimState, SimAction, SimNode, Metrics, RawSimEvent } from '../types';

const MAX_EVENTS = 100;

const initialState: SimState = {
  connected: false,
  running: false,
  currentTime: 0,
  nodes: new Map(),
  links: [],
  metrics: null,
  events: [],
  eventCounter: 0,
};

function simReducer(state: SimState, action: SimAction): SimState {
  switch (action.type) {
    case 'CONNECTED':
      return { ...state, connected: true, running: true };

    case 'DISCONNECTED':
      return { ...state, connected: false, running: false };

    case 'SIM_ENDED':
      return { ...state, running: false };

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
        // Auto-create if we see a move event without a prior join
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

    default:
      return state;
  }
}

function parseEvent(raw: RawSimEvent): SimAction[] {
  const actions: SimAction[] = [];

  // Always add to event log
  const { type, timestamp_us, ...rest } = raw;
  actions.push({
    type: 'ADD_EVENT',
    event: { type, timestamp_us, details: rest },
  });

  // State machine updates
  switch (type) {
    case 'node_joined': {
      const node: SimNode = {
        id: raw.node as string,
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
      const delivered = (raw.delivered as number) ?? 0;
      const dropped = (raw.dropped as number) ?? 0;
      const metrics: Metrics = {
        timestamp_us,
        activeNodes: (raw.active_nodes as number) ?? 0,
        totalPackets,
        delivered,
        dropped,
        avgLatencyMs: (raw.avg_latency_ms as number) ?? 0,
        deliveryRate: totalPackets > 0 ? (delivered / totalPackets) * 100 : 0,
      };
      actions.push({ type: 'UPDATE_METRICS', metrics });
      break;
    }
    case 'sim_ended': {
      actions.push({ type: 'SIM_ENDED' });
      break;
    }
  }

  return actions;
}

export function useSimulation() {
  const [state, dispatch] = useReducer(simReducer, initialState);
  const wsRef = useRef<WebSocket | null>(null);

  useEffect(() => {
    const wsUrl = `ws://${window.location.host}`;
    console.log(`[useSimulation] Connecting to ${wsUrl}`);

    let ws: WebSocket;
    let reconnectTimer: ReturnType<typeof setTimeout>;

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
          const actions = parseEvent(raw);
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

  return { state, ws: wsRef.current };
}
