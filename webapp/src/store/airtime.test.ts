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

  it('derives usedPct from the consumed fraction of each lane', () => {
    const status = normalizeAirtime({
      critical_remaining_ms: 9000,   // 75% used of 36000
      critical_max_ms: 36000,
      normal_remaining_ms: 18000,    // 0% used
      normal_max_ms: 18000,
      broadcast_remaining_ms: 0,     // 100% used
      broadcast_max_ms: 18000,
    });
    const byName = Object.fromEntries(status.tiers.map(t => [t.name, t.usedPct]));
    expect(byName).toEqual({ critical: 75, normal: 0, broadcast: 100 });
  });

  it('reports 0% used for a lane with no budget instead of dividing by zero', () => {
    const status = normalizeAirtime({
      critical_remaining_ms: 0,
      critical_max_ms: 0,
      normal_remaining_ms: 0,
      normal_max_ms: 0,
      broadcast_remaining_ms: 0,
      broadcast_max_ms: 0,
    });
    expect(status.tiers.every(t => t.usedPct === 0)).toBe(true);
  });
});
