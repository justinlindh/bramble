import { describe, it, expect } from 'vitest';
import { mergeNearby } from '../nearbyNodes';
import type { SavedDevice } from '../deviceBook';
import type { DiscoveredNode } from '../../types/desktop';

const saved = (address: string, name: string): SavedDevice => ({
  address, name, lastIp: '192.168.1.9', transport: 'wifi', remember: true, lastConnectedAt: 1,
});

const disc = (over: Partial<DiscoveredNode>): DiscoveredNode => ({
  hostname: 'bramble-6eee', ip: '192.168.1.21', ...over,
});

describe('mergeNearby', () => {
  it('exact-matches by full address and uses the saved name', () => {
    const [n] = mergeNearby(
      [disc({ addrHex: 'F2BE6EEE', name: 'Garage (fw)' })],
      [saved('F2BE6EEE', 'Garage')],
    );
    expect(n.saved?.address).toBe('F2BE6EEE');
    expect(n.probableSaved).toBeUndefined();
    expect(n.displayName).toBe('Garage');
  });

  it('uses the TXT name for unknown nodes', () => {
    const [n] = mergeNearby([disc({ addrHex: '11112222', name: 'Attic' })], []);
    expect(n.saved).toBeUndefined();
    expect(n.displayName).toBe('Attic');
    expect(n.txtName).toBe('Attic');
  });

  it('falls back to hostname when there is no name at all', () => {
    const [n] = mergeNearby([disc({})], []);
    expect(n.displayName).toBe('bramble-6eee');
  });

  it('suffix-matches old firmware (no TXT) against a single book entry', () => {
    const [n] = mergeNearby([disc({})], [saved('F2BE6EEE', 'Garage')]);
    expect(n.probableSaved?.address).toBe('F2BE6EEE');
    expect(n.saved).toBeUndefined();
    expect(n.displayName).toBe('Garage');
  });

  it('does not suffix-match when the suffix is ambiguous', () => {
    const [n] = mergeNearby(
      [disc({})],
      [saved('F2BE6EEE', 'Garage'), saved('AAAA6EEE', 'Attic')],
    );
    expect(n.probableSaved).toBeUndefined();
    expect(n.displayName).toBe('bramble-6eee');
  });

  it('never suffix-matches when TXT addr is present (exact info wins)', () => {
    const [n] = mergeNearby([disc({ addrHex: '11116EEE' })], [saved('F2BE6EEE', 'Garage')]);
    expect(n.saved).toBeUndefined();
    expect(n.probableSaved).toBeUndefined();
  });

  it('sorts exact matches first, then probable, then unknown, alphabetically within groups', () => {
    const nodes = mergeNearby(
      [
        disc({ hostname: 'bramble-0001', ip: '10.0.0.1', name: 'Zeta' }),
        disc({ hostname: 'bramble-6eee', ip: '10.0.0.2' }),
        disc({ hostname: 'bramble-0002', ip: '10.0.0.3', addrHex: 'AAAA0002' }),
      ],
      [saved('F2BE6EEE', 'Garage'), saved('AAAA0002', 'Attic')],
    );
    expect(nodes.map(n => n.displayName)).toEqual(['Attic', 'Garage', 'Zeta']);
  });
});
