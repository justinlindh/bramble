import { describe, it, expect } from 'vitest';
import { serviceToNode, nodeKey, upsertNode, removeService, type RawService } from '../discoveryCore';
import type { DiscoveredNode } from '../../types/desktop';

const fullService: RawService = {
  host: 'bramble-beef.local',
  addresses: ['fe80::1', '192.0.2.21'],
  txt: { addr: 'deadbeef', name: 'Node A' },
};

describe('serviceToNode', () => {
  it('maps a full service with TXT records', () => {
    expect(serviceToNode(fullService)).toEqual({
      addrHex: 'DEADBEEF',
      name: 'Node A',
      hostname: 'bramble-beef',
      ip: '192.0.2.21',
    });
  });

  it('handles old firmware without TXT records', () => {
    const svc: RawService = { host: 'bramble-beef.local', addresses: ['192.0.2.21'] };
    expect(serviceToNode(svc)).toEqual({
      addrHex: undefined,
      name: undefined,
      hostname: 'bramble-beef',
      ip: '192.0.2.21',
    });
  });

  it('rejects a malformed TXT addr instead of trusting it', () => {
    const svc: RawService = { ...fullService, txt: { addr: 'nothex!!', name: 'Node A' } };
    expect(serviceToNode(svc)?.addrHex).toBeUndefined();
  });

  it('returns null without an IPv4 address', () => {
    expect(serviceToNode({ host: 'bramble-beef.local', addresses: ['fe80::1'] })).toBeNull();
    expect(serviceToNode({ host: 'bramble-beef.local' })).toBeNull();
  });

  it('returns null without a host', () => {
    expect(serviceToNode({ addresses: ['192.0.2.21'] })).toBeNull();
  });
});

describe('snapshot maintenance', () => {
  const node = serviceToNode(fullService) as DiscoveredNode;

  it('keys by full address when present, hostname otherwise', () => {
    expect(nodeKey(node)).toBe('DEADBEEF');
    expect(nodeKey({ ...node, addrHex: undefined })).toBe('bramble-beef');
  });

  it('upsert replaces an existing entry instead of duplicating (DHCP renew)', () => {
    const s1 = upsertNode([], node);
    const s2 = upsertNode(s1, { ...node, ip: '192.0.2.99' });
    expect(s2).toHaveLength(1);
    expect(s2[0].ip).toBe('192.0.2.99');
  });

  it('upsert keeps distinct nodes', () => {
    const other: DiscoveredNode = { addrHex: '11112222', hostname: 'bramble-2222', ip: '192.0.2.30' };
    expect(upsertNode(upsertNode([], node), other)).toHaveLength(2);
  });

  it('removeService drops the matching entry using TXT addr', () => {
    const s = upsertNode([], node);
    expect(removeService(s, fullService)).toHaveLength(0);
  });

  it('removeService falls back to hostname when the down event has no TXT', () => {
    const bare: DiscoveredNode = { hostname: 'bramble-beef', ip: '192.0.2.21' };
    const s = upsertNode([], bare);
    expect(removeService(s, { host: 'bramble-beef.local' })).toHaveLength(0);
  });

  it('removeService ignores unmatchable services', () => {
    const s = upsertNode([], node);
    expect(removeService(s, {})).toHaveLength(1);
  });

  it('removeService drops an addrHex-keyed node when the down event has no TXT', () => {
    const s = upsertNode([], node);
    expect(removeService(s, { host: 'bramble-beef.local' })).toHaveLength(0);
  });
});
