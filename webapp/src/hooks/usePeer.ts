import { useEffect } from 'react';
import { useStore } from '../store/index';
import { resolvePeerName } from '../store/peerName';
import { loadPeerVerification } from '../store/actions';
import { formatAddr0x, formatAddrShort } from '../utils/address';
import { formatAge } from './useAgeTick';

export type PeerStatus = 'online' | 'reachable' | 'unknown';

const ONLINE_THRESHOLD_MS = 90_000;   // 3x beacon interval (30s)

/** Resolve a peer address to a display name + presence status */
export function usePeerInfo(addr: number) {
  const resolvedName = useStore((s) =>
    resolvePeerName(addr, s.peerNames, s.peerLocations),
  );
  const neighbor = useStore(s => s.neighbors?.find(n => n.addr === addr));
  const route = useStore(s => s.routes.find(r => r.dest === addr));

  let status: PeerStatus = 'unknown';
  if (neighbor) {
    status = neighbor.lastHeardMs < ONLINE_THRESHOLD_MS ? 'online' : 'reachable';
  } else if (route && route.state === 'active') {
    status = 'reachable';
  }

  const shortHex = formatAddrShort(addr);
  const fullHex = formatAddr0x(addr);
  const displayName = resolvedName ?? shortHex;

  let lastSeen: string | null = null;
  if (neighbor && status !== 'online') {
    lastSeen = formatAge(neighbor.lastHeardMs);
  }

  return { name: resolvedName, displayName, shortHex, fullHex, status, lastSeen };
}

/** Cached SAS-verification state for a peer, lazily loaded once per peer
 * (does not refresh on every mount; VerifySafetyNumberPanel's always-refresh
 * variant loads separately since it intentionally reloads on open). */
export function usePeerVerification(addr: number) {
  const verification = useStore(s => s.peerVerifications.get(addr));

  useEffect(() => {
    if (!verification) loadPeerVerification(addr).catch(() => {});
  }, [addr, verification]);

  return verification;
}

export const STATUS_COLORS: Record<PeerStatus, string> = {
  online: '#22c55e',    // green
  reachable: '#eab308', // yellow
  unknown: '#6b7280',   // gray
};
