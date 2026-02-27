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
});
