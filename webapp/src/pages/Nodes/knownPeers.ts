import { parseAddr } from '../../lib/addr';
import type { Neighbor, PeerLocation, Route } from '../../types/bramble';

export interface KnownPeer {
  addr: number;
  hasNeighbor: boolean;
  hasRoute: boolean;
  peerLocation?: PeerLocation;
}

export function buildKnownPeers(neighbors: Neighbor[], routes: Route[], peerLocations: PeerLocation[]): KnownPeer[] {
  const byAddr = new Map<number, KnownPeer>();

  for (const n of neighbors) {
    const addr = parseAddr(n.addr);
    if (!addr) continue;
    byAddr.set(addr, { addr, hasNeighbor: true, hasRoute: false, peerLocation: byAddr.get(addr)?.peerLocation });
  }

  for (const r of routes) {
    const addr = parseAddr(r.dest as unknown as number | string);
    if (!addr) continue;
    const existing = byAddr.get(addr);
    byAddr.set(addr, {
      addr,
      hasNeighbor: existing?.hasNeighbor ?? false,
      hasRoute: true,
      peerLocation: existing?.peerLocation,
    });
  }

  for (const p of peerLocations) {
    const addr = parseAddr(p.addr);
    if (!addr) continue;
    const existing = byAddr.get(addr);
    byAddr.set(addr, {
      addr,
      hasNeighbor: existing?.hasNeighbor ?? false,
      hasRoute: existing?.hasRoute ?? false,
      peerLocation: p,
    });
  }

  return [...byAddr.values()].sort((a, b) => a.addr - b.addr);
}
