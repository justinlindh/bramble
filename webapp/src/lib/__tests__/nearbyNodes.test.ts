import { describe, it, expect } from 'vitest';
import { mergeNearby } from '../nearbyNodes';
import type { SavedDevice } from '../deviceBook';
import type { DiscoveredNode } from '../../types/desktop';

const saved = (address: string, name: string): SavedDevice => ({
  address, name, lastIp: '192.0.2.9', transport: 'wifi', remember: true, lastConnectedAt: 1,
});

const disc = (over: Partial<DiscoveredNode>): DiscoveredNode => ({
  hostname: 'bramble-beef', ip: '192.0.2.21', ...over,
});

describe('mergeNearby', () => {
  it('exact-matches by full address and uses the saved name', () => {
    const [n] = mergeNearby(
      [disc({ addrHex: 'DEADBEEF', name: 'Node A (fw)' })],
      [saved('DEADBEEF', 'Node A')],
    );
    expect(n.saved?.address).toBe('DEADBEEF');
    expect(n.probableSaved).toBeUndefined();
    expect(n.displayName).toBe('Node A');
  });

  it('uses the TXT name for unknown nodes', () => {
    const [n] = mergeNearby([disc({ addrHex: '11112222', name: 'Node B' })], []);
    expect(n.saved).toBeUndefined();
    expect(n.displayName).toBe('Node B');
    expect(n.txtName).toBe('Node B');
  });

  it('falls back to hostname when there is no name at all', () => {
    const [n] = mergeNearby([disc({})], []);
    expect(n.displayName).toBe('bramble-beef');
  });

  it('suffix-matches old firmware (no TXT) against a single book entry', () => {
    const [n] = mergeNearby([disc({})], [saved('DEADBEEF', 'Node A')]);
    expect(n.probableSaved?.address).toBe('DEADBEEF');
    expect(n.saved).toBeUndefined();
    expect(n.displayName).toBe('Node A');
  });

  it('does not suffix-match when the suffix is ambiguous', () => {
    const [n] = mergeNearby(
      [disc({})],
      [saved('DEADBEEF', 'Node A'), saved('AAAABEEF', 'Node B')],
    );
    expect(n.probableSaved).toBeUndefined();
    expect(n.displayName).toBe('bramble-beef');
  });

  it('never suffix-matches when TXT addr is present (exact info wins)', () => {
    const [n] = mergeNearby([disc({ addrHex: '11116EEE' })], [saved('DEADBEEF', 'Node A')]);
    expect(n.saved).toBeUndefined();
    expect(n.probableSaved).toBeUndefined();
  });

  it('sorts exact matches first, then probable, then unknown, alphabetically within groups', () => {
    const nodes = mergeNearby(
      [
        disc({ hostname: 'bramble-0001', ip: '192.0.2.1', name: 'Zeta' }),
        disc({ hostname: 'bramble-beef', ip: '192.0.2.2' }),
        disc({ hostname: 'bramble-0002', ip: '192.0.2.3', addrHex: 'AAAA0002' }),
      ],
      [saved('DEADBEEF', 'Node A'), saved('AAAA0002', 'Node B')],
    );
    expect(nodes.map(n => n.displayName)).toEqual(['Node B', 'Node A', 'Zeta']);
  });
});
