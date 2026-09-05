import { describe, it, expect } from 'vitest';
import { normalizeAirtime } from './actions';

describe('normalizeAirtime', () => {
  it('keeps three lanes when the firmware omits the receipt lane', () => {
    const status = normalizeAirtime({
      critical_remaining_ms: 30000,
      critical_max_ms: 36000,
      normal_remaining_ms: 9000,
      normal_max_ms: 18000,
      broadcast_remaining_ms: 18000,
      broadcast_max_ms: 18000,
    });
    expect(status.tiers.map(t => t.name)).toEqual(['critical', 'normal', 'broadcast']);
  });

  it('adds the receipt lane when the firmware reports it (PR #82 four-lane shape)', () => {
    const status = normalizeAirtime({
      critical_remaining_ms: 30000,
      critical_max_ms: 36000,
      normal_remaining_ms: 9000,
      normal_max_ms: 18000,
      broadcast_remaining_ms: 18000,
      broadcast_max_ms: 18000,
      receipt_remaining_ms: 6000,
      receipt_max_ms: 12000,
    });
    expect(status.tiers.map(t => t.name)).toEqual(['critical', 'normal', 'broadcast', 'receipt']);
    const receipt = status.tiers.find(t => t.name === 'receipt')!;
    expect(receipt.maxMs).toBe(12000);
    expect(receipt.remainingMs).toBe(6000);
    expect(receipt.usedPct).toBe(50);
  });
});
