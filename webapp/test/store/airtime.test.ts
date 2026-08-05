import { describe, it, expect } from 'vitest';
import { normalizeAirtime } from '../../src/store/actions';
import { formatRefill } from '../../src/pages/Stats/AirtimeCard';

// These exercise the real normalizeAirtime (store/actions/telemetry.ts) and
// formatRefill (Stats/AirtimeCard.tsx). Earlier this file kept hand-copied
// mirrors of both, which silently drifted from production (the mirror never
// grew the receipt lane); importing the real implementations removes that
// drift hazard.

describe('normalizeAirtime: firmware flat format', () => {
  it('maps firmware fields to tier objects', () => {
    const raw = {
      critical_remaining_ms: 36000,
      normal_remaining_ms: 18000,
      broadcast_remaining_ms: 18000,
      critical_max_ms: 36000,
      normal_max_ms: 18000,
      broadcast_max_ms: 18000,
      next_refill_ms: 3600000,
    };

    const result = normalizeAirtime(raw);

    expect(result.tiers).toHaveLength(3);
    expect(result.tiers[0].name).toBe('critical');
    expect(result.tiers[1].name).toBe('normal');
    expect(result.tiers[2].name).toBe('broadcast');
  });

  it('computes correct remaining percentages when full', () => {
    const raw = {
      critical_remaining_ms: 36000,
      normal_remaining_ms: 18000,
      broadcast_remaining_ms: 18000,
      critical_max_ms: 36000,
      normal_max_ms: 18000,
      broadcast_max_ms: 18000,
      next_refill_ms: 3600000,
    };

    const result = normalizeAirtime(raw);

    // All full → 0% used
    expect(result.tiers[0].usedPct).toBe(0);
    expect(result.tiers[1].usedPct).toBe(0);
    expect(result.tiers[2].usedPct).toBe(0);
  });

  it('computes correct percentages when partially used', () => {
    const raw = {
      critical_remaining_ms: 18000, // half used
      normal_remaining_ms: 9000,    // half used
      broadcast_remaining_ms: 9000, // half used
      critical_max_ms: 36000,
      normal_max_ms: 18000,
      broadcast_max_ms: 18000,
      next_refill_ms: 1800000,
    };

    const result = normalizeAirtime(raw);

    expect(result.tiers[0].usedPct).toBe(50);
    expect(result.tiers[1].usedPct).toBe(50);
    expect(result.tiers[2].usedPct).toBe(50);
  });

  it('broadcast remaining is NOT zero when firmware sends full budget', () => {
    // This was the original bug: tier mapping caused broadcast to always be 0
    const raw = {
      critical_remaining_ms: 36000,
      normal_remaining_ms: 18000,
      broadcast_remaining_ms: 18000,
      critical_max_ms: 36000,
      normal_max_ms: 18000,
      broadcast_max_ms: 18000,
      next_refill_ms: 3600000,
    };

    const result = normalizeAirtime(raw);
    const broadcast = result.tiers.find(t => t.name === 'broadcast')!;

    expect(broadcast.remainingMs).toBe(18000);
    expect(broadcast.maxMs).toBe(18000);
    expect(broadcast.usedPct).toBe(0);
  });

  it('refillAtMs is in the future when next_refill_ms > 0', () => {
    const now = Date.now();
    const raw = {
      critical_remaining_ms: 36000,
      normal_remaining_ms: 18000,
      broadcast_remaining_ms: 18000,
      critical_max_ms: 36000,
      normal_max_ms: 18000,
      broadcast_max_ms: 18000,
      next_refill_ms: 3600000,
    };

    const result = normalizeAirtime(raw);

    // All tiers should have refillAtMs ~1 hour in the future
    for (const tier of result.tiers) {
      expect(tier.refillAtMs).toBeGreaterThan(now);
      expect(tier.refillAtMs).toBeLessThanOrEqual(now + 3600000 + 100); // small tolerance
    }
  });

  it('refillAtMs defaults to 1 hour when next_refill_ms is missing', () => {
    const now = Date.now();
    const raw = {
      critical_remaining_ms: 36000,
      normal_remaining_ms: 18000,
      broadcast_remaining_ms: 18000,
      critical_max_ms: 36000,
      normal_max_ms: 18000,
      broadcast_max_ms: 18000,
      // next_refill_ms intentionally omitted
    };

    const result = normalizeAirtime(raw);

    for (const tier of result.tiers) {
      expect(tier.refillAtMs).toBeGreaterThan(now + 3500000);
    }
  });

  it('treats zero next_refill_ms as a full interval (just refilled)', () => {
    // When firmware just triggered a refill, next_refill_ms = 0. That means
    // "just refilled, next one in a full interval", not "refills now".
    const now = Date.now();
    const raw = {
      critical_remaining_ms: 36000,
      normal_remaining_ms: 18000,
      broadcast_remaining_ms: 18000,
      critical_max_ms: 36000,
      normal_max_ms: 18000,
      broadcast_max_ms: 18000,
      next_refill_ms: 0,
    };

    const result = normalizeAirtime(raw);

    for (const tier of result.tiers) {
      expect(tier.refillAtMs).toBeGreaterThan(now + 3500000);
    }
  });

  it('defaults missing remaining fields to 0', () => {
    const raw = {
      // Only max fields present
      critical_max_ms: 36000,
      normal_max_ms: 18000,
      broadcast_max_ms: 18000,
      next_refill_ms: 3600000,
    };

    const result = normalizeAirtime(raw);

    expect(result.tiers[0].remainingMs).toBe(0);
    expect(result.tiers[1].remainingMs).toBe(0);
    expect(result.tiers[2].remainingMs).toBe(0);
    expect(result.tiers[0].usedPct).toBe(100);
  });

  it('adds the receipt lane when the firmware reports it', () => {
    const raw = {
      critical_remaining_ms: 36000,
      normal_remaining_ms: 18000,
      broadcast_remaining_ms: 18000,
      critical_max_ms: 36000,
      normal_max_ms: 18000,
      broadcast_max_ms: 18000,
      receipt_remaining_ms: 6000,
      receipt_max_ms: 12000,
      next_refill_ms: 3600000,
    };

    const result = normalizeAirtime(raw);

    expect(result.tiers.map(t => t.name)).toEqual(['critical', 'normal', 'broadcast', 'receipt']);
    const receipt = result.tiers.find(t => t.name === 'receipt')!;
    expect(receipt.remainingMs).toBe(6000);
    expect(receipt.usedPct).toBe(50);
  });
});

describe('normalizeAirtime: mock/tiers format', () => {
  it('passes through tiers format unchanged', () => {
    const raw = {
      tiers: [
        { name: 'critical', remainingMs: 9200, maxMs: 10000, usedPct: 8, refillAtMs: Date.now() + 55000 },
        { name: 'normal', remainingMs: 41000, maxMs: 60000, usedPct: 32, refillAtMs: Date.now() + 120000 },
        { name: 'broadcast', remainingMs: 22500, maxMs: 30000, usedPct: 25, refillAtMs: Date.now() + 300000 },
      ],
    };

    const result = normalizeAirtime(raw);
    expect(result).toBe(raw); // same reference: passthrough
  });
});

describe('formatRefill', () => {
  it('shows "now" when refillAtMs is in the past', () => {
    expect(formatRefill(Date.now() - 1000)).toBe('now');
  });

  it('shows seconds for near-future refills', () => {
    const result = formatRefill(Date.now() + 30000);
    expect(result).toMatch(/^in \d+s$/);
  });

  it('shows minutes for longer refills', () => {
    const result = formatRefill(Date.now() + 3600000);
    expect(result).toMatch(/^in \d+m \d+s$/);
  });

  it('shows "now" when refillAtMs equals Date.now()', () => {
    // Edge case: exact match may show "now" due to timing
    const result = formatRefill(Date.now());
    expect(result).toBe('now');
  });
});
