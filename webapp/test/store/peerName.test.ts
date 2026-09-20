import { describe, it, expect } from 'vitest';
import { resolvePeerName } from '../../src/store/peerName';
import type { PeerLocation } from '../../src/types/bramble';

const ADDR = 0xcc04;

function makePeerLocation(addr: number, name: string): PeerLocation {
  return {
    addr,
    name,
    tier: 'presence',
    position: null,
    online: true,
    lastUpdatedMs: Date.now(),
  };
}

describe('resolvePeerName', () => {
  it('returns undefined when no name sources are available', () => {
    expect(resolvePeerName(ADDR, new Map(), [], undefined)).toBeUndefined();
  });

  it('returns undefined when maps are undefined', () => {
    expect(resolvePeerName(ADDR, undefined, undefined, undefined)).toBeUndefined();
  });

  it('resolves a name from location telemetry when no other name exists', () => {
    const locs: PeerLocation[] = [makePeerLocation(ADDR, 'Alice')];
    expect(resolvePeerName(ADDR, new Map(), locs, undefined)).toBe('Alice');
  });

  it('resolves a name from peerNames when no location name exists', () => {
    const names = new Map([[ADDR, 'Bob']]);
    expect(resolvePeerName(ADDR, names, [], undefined)).toBe('Bob');
  });

  it('prefers a contact name over a location telemetry name', () => {
    const contacts = new Map([[ADDR, 'Bob']]);
    const locs: PeerLocation[] = [makePeerLocation(ADDR, 'Alice')];
    expect(resolvePeerName(ADDR, new Map(contacts), locs, contacts)).toBe('Bob');
  });

  it('prefers a location telemetry name over a learned name', () => {
    const names = new Map([[ADDR, 'Bob']]);
    const locs: PeerLocation[] = [makePeerLocation(ADDR, 'Alice')];
    expect(resolvePeerName(ADDR, names, locs, undefined)).toBe('Alice');
  });

  it('skips a location entry with a blank name and falls back to peerNames', () => {
    const names = new Map([[ADDR, 'Bob']]);
    const locs: PeerLocation[] = [makePeerLocation(ADDR, '   ')];
    expect(resolvePeerName(ADDR, names, locs, undefined)).toBe('Bob');
  });

  it('returns undefined for a route-only peer with no name in any source', () => {
    // Simulate a peer known only via routing (addr present but no names)
    const names = new Map<number, string>();
    const locs: PeerLocation[] = [];
    expect(resolvePeerName(ADDR, names, locs, undefined)).toBeUndefined();
  });

  it('ignores location entries for a different address', () => {
    const locs: PeerLocation[] = [makePeerLocation(0x1234, 'Wrong Peer')];
    expect(resolvePeerName(ADDR, new Map(), locs, undefined)).toBeUndefined();
  });
});
