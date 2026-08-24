import { describe, expect, it } from 'vitest';
import { buildKnownPeers } from '../../src/pages/Nodes/knownPeers';

describe('buildKnownPeers', () => {
  it('includes peer location entries even when neighbors/routes are empty', () => {
    const peers = buildKnownPeers([], [], [
      { addr: 0xd4813079, name: '', tier: 'full', position: { lat: 1, lon: 2, alt: 0, accuracy: 0, speed: 0, heading: 0, timestampMs: Date.now() }, online: true, lastUpdatedMs: Date.now() },
    ]);

    expect(peers).toHaveLength(1);
    expect(peers[0].addr).toBe(0xd4813079);
  });

  it('merges a peer present in several source sets into one entry, sorted by address', () => {
    const shared = 0x1234abcd;
    const peers = buildKnownPeers(
      [{ addr: shared, rssi: -70, snr: 10, lastHeardMs: 1200 }],
      [
        { dest: shared, nextHop: shared, hopCount: 1, metric: 10, state: 'active', lastUsedMs: 50 },
        { dest: 0x0000aaaa, nextHop: shared, hopCount: 2, metric: 20, state: 'active', lastUsedMs: 60 },
      ],
      [{ addr: shared, name: '', tier: 'presence', position: null, online: true, lastUpdatedMs: 500 }],
    );

    expect(peers.map((p) => p.addr)).toEqual([0x0000aaaa, shared]);
    const merged = peers[1];
    expect(merged.hasRoute).toBe(true);
    expect(merged.peerLocation).toBeDefined();
    expect(merged.neighbor?.lastHeardMs).toBe(1200);
    expect(peers[0].neighbor).toBeUndefined();
  });
});
