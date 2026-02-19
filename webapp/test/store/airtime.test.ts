import { describe, it, expect, vi, beforeEach } from 'vitest';

// We need to test normalizeAirtime which is not exported.
// Extract the logic into a testable function and test the behavior.
// For now, we inline the normalizer logic to test it directly.

interface AirtimeTier {
  name: 'critical' | 'normal' | 'broadcast';
  remainingMs: number;
  maxMs: number;
  usedPct: number;
  refillAtMs: number;
}

interface AirtimeStatus {
  tiers: [AirtimeTier, AirtimeTier, AirtimeTier];
}

const REFILL_INTERVAL_MS = 3600000;

// Mirror of normalizeAirtime from actions.ts (must stay in sync)
function normalizeAirtime(raw: any): AirtimeStatus {
  if (raw.tiers) return raw as AirtimeStatus;
  const nextRefillMs = raw.next_refill_ms ?? 3600000;
  const refillAtMs = Date.now() + (nextRefillMs > 0 ? nextRefillMs : REFILL_INTERVAL_MS);
  return {
    tiers: [
      { name: 'critical', remainingMs: raw.critical_remaining_ms ?? 0, maxMs: raw.critical_max_ms ?? 36000, usedPct: 0, refillAtMs },
      { name: 'normal', remainingMs: raw.normal_remaining_ms ?? 0, maxMs: raw.normal_max_ms ?? 18000, usedPct: 0, refillAtMs },
      { name: 'broadcast', remainingMs: raw.broadcast_remaining_ms ?? 0, maxMs: raw.broadcast_max_ms ?? 18000, usedPct: 0, refillAtMs },
    ].map(t => ({ ...t, usedPct: t.maxMs > 0 ? Math.round(100 * (t.maxMs - t.remainingMs) / t.maxMs) : 0 })) as [AirtimeTier, AirtimeTier, AirtimeTier],
  };
}

// Mirror of formatRefill from AirtimeCard.tsx
function formatRefill(refillAtMs: number): string {
  const diffMs = refillAtMs - Date.now();
  if (diffMs <= 0) return 'now';
  const s = Math.ceil(diffMs / 1000);
  if (s >= 60) return `in ${Math.floor(s / 60)}m ${s % 60}s`;
  return `in ${s}s`;
}

describe('normalizeAirtime — firmware flat format', () => {
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

  it('handles zero next_refill_ms (just refilled, should show ~1hr)', () => {
    // When firmware just triggered a refill, next_refill_ms = 0
    // This SHOULD mean "just refilled, next one in 1 hour"
    // But the current code computes refillAtMs = Date.now() + 0 = now → "refills now"
    // This is THE BUG: next_refill_ms=0 means "refill is overdue/just happened"
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

    // next_refill_ms=0 after a refill means the budget was JUST refilled
    // The refillAtMs should be ~1 hour from now, not "now"
    for (const tier of result.tiers) {
      // next_refill_ms=0 should be treated as "just refilled, next in 1hr"
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
});

describe('normalizeAirtime — mock/tiers format', () => {
  it('passes through tiers format unchanged', () => {
    const raw = {
      tiers: [
        { name: 'critical', remainingMs: 9200, maxMs: 10000, usedPct: 8, refillAtMs: Date.now() + 55000 },
        { name: 'normal', remainingMs: 41000, maxMs: 60000, usedPct: 32, refillAtMs: Date.now() + 120000 },
        { name: 'broadcast', remainingMs: 22500, maxMs: 30000, usedPct: 25, refillAtMs: Date.now() + 300000 },
      ],
    };

    const result = normalizeAirtime(raw);
    expect(result).toBe(raw); // same reference — passthrough
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
