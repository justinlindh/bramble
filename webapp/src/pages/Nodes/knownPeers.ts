import { parseAddr } from '../../lib/addr';
import type { Neighbor, PeerLocation, Route } from '../../types/bramble';

export interface KnownPeer {
  addr: number;
  /** Set when the peer is a live radio neighbor; carries the neighbor's own
   *  fields (rssi, snr, lastHeardMs) that no other source set provides. */
  neighbor?: Neighbor;
  hasRoute: boolean;
  peerLocation?: PeerLocation;
}

export function buildKnownPeers(neighbors: Neighbor[], routes: Route[], peerLocations: PeerLocation[]): KnownPeer[] {
  const byAddr = new Map<number, KnownPeer>();

  for (const n of neighbors) {
    const addr = parseAddr(n.addr);
    if (!addr) continue;
    byAddr.set(addr, { addr, neighbor: n, hasRoute: false });
  }

  for (const r of routes) {
    const addr = parseAddr(r.dest as unknown as number | string);
    if (!addr) continue;
    const existing = byAddr.get(addr);
    byAddr.set(addr, {
      addr,
      neighbor: existing?.neighbor,
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
      neighbor: existing?.neighbor,
      hasRoute: existing?.hasRoute ?? false,
      peerLocation: p,
    });
  }

  return [...byAddr.values()].sort((a, b) => a.addr - b.addr);
}
