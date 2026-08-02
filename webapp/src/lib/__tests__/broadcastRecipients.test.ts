import { describe, it, expect } from 'vitest';
import { mergeBroadcastRecipient } from '../broadcastRecipients';
import type { BroadcastDeliveryRecipient } from '../../types/bramble';

const r = (
  addr: number,
  deliveredAtMs: number,
  overrides: Partial<BroadcastDeliveryRecipient> = {},
): BroadcastDeliveryRecipient => ({
  addr,
  status: 'delivered',
  hopCount: 1,
  deliveredAtMs,
  ...overrides,
});

describe('mergeBroadcastRecipient', () => {
  it('appends a recipient not yet present', () => {
    const base = [r(1, 100)];
    const next = mergeBroadcastRecipient(base, r(2, 200));
    expect(next).toHaveLength(2);
    expect(next.map(x => x.addr)).toEqual([1, 2]);
  });

  it('returns the same array reference when the incoming report is stale', () => {
    const base = [r(1, 200)];
    const next = mergeBroadcastRecipient(base, r(1, 100, { status: 'failed' }));
    expect(next).toBe(base);
  });

  it('replaces an existing recipient when the incoming report is newer', () => {
    const base = [r(1, 100, { status: 'pending', hopCount: 3 })];
    const next = mergeBroadcastRecipient(base, r(1, 200, { status: 'delivered', hopCount: 5 }));
    expect(next).not.toBe(base);
    expect(next).toHaveLength(1);
    expect(next[0]).toEqual({ addr: 1, status: 'delivered', hopCount: 5, deliveredAtMs: 200 });
  });

  it('merges fields so an equal timestamp still applies the incoming values', () => {
    const base = [r(1, 100, { status: 'pending' })];
    const next = mergeBroadcastRecipient(base, r(1, 100, { status: 'delivered' }));
    expect(next).not.toBe(base);
    expect(next[0].status).toBe('delivered');
  });

  it('does not disturb unrelated recipients', () => {
    const base = [r(1, 100), r(2, 100)];
    const next = mergeBroadcastRecipient(base, r(2, 200, { status: 'failed' }));
    expect(next[0]).toEqual(base[0]);
    expect(next[1].status).toBe('failed');
  });
});
