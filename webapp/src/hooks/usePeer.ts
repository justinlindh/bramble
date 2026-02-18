import { useStore } from '../store/index';

export type PeerStatus = 'online' | 'reachable' | 'unknown';

const ONLINE_THRESHOLD_MS = 90_000;   // 3x beacon interval (30s)

/** Resolve a peer address to a display name + presence status */
export function usePeerInfo(addr: number) {
  const name = useStore(s => s.peerNames.get(addr));
  const neighbor = useStore(s => s.neighbors.find(n => n.addr === addr));
  const route = useStore(s => s.routes.find(r => r.dest === addr));

  let status: PeerStatus = 'unknown';
  if (neighbor) {
    status = neighbor.lastHeardMs < ONLINE_THRESHOLD_MS ? 'online' : 'reachable';
  } else if (route && route.state === 'active') {
    status = 'reachable';
  }

  const shortHex = `0x${addr.toString(16).toUpperCase().padStart(8, '0').slice(-4)}`;
  const fullHex = `0x${addr.toString(16).toUpperCase().padStart(8, '0')}`;
  const displayName = name || shortHex;

  let lastSeen: string | null = null;
  if (neighbor && status !== 'online') {
    const secs = Math.round(neighbor.lastHeardMs / 1000);
    if (secs < 60) lastSeen = `${secs}s ago`;
    else if (secs < 3600) lastSeen = `${Math.round(secs / 60)}m ago`;
    else lastSeen = `${Math.round(secs / 3600)}h ago`;
  }

  return { name, displayName, shortHex, fullHex, status, lastSeen };
}

export const STATUS_COLORS: Record<PeerStatus, string> = {
  online: '#22c55e',    // green
  reachable: '#eab308', // yellow
  unknown: '#6b7280',   // gray
};
