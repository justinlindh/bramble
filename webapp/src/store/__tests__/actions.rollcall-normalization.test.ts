import { describe, expect, it } from 'vitest';
import { normalizeRollCallLedger } from '../actions';

describe('normalizeRollCallLedger', () => {
  it('maps the firmware ledger onto the app shape', () => {
    const led = normalizeRollCallLedger({
      active: true,
      open: true,
      rollcall_id: '0000BEEF',
      text: 'sound off',
      rounds_sent: 2,
      rounds_total: 3,
      window_ms: 135_000,
      elapsed_ms: 42_000,
      min_interval_ms: 300_000,
      max_text_bytes: 48,
      anchored: true,
      expected: 3,
      responded: 2,
      unattested: 1,
      overflow: 0,
      late: 0,
      pending_dropped: 0,
      answer_limited: 2,
      missing_count: 1,
      missing: ['0000000D'],
      responders: [
        { address: '0000000B', responded: true, at_ms: 3200, round: 1, hops: 2, path: ['AABBCCDD', '0000000B'] },
      ],
    });

    expect(led.rollcallId).toBe('0000BEEF');
    expect(led.roundsSent).toBe(2);
    expect(led.minIntervalMs).toBe(300_000);
    expect(led.anchored).toBe(true);
    expect(led.answerLimited).toBe(2);
    expect(led.missing).toEqual([0x0000000d]);
    expect(led.responders).toEqual([
      { addr: 0x0000000b, responded: true, atMs: 3200, round: 1, relayPath: [0xaabbccdd, 0x0000000b] },
    ]);
  });

  it('reads an inactive ledger without inventing a roll-call', () => {
    const led = normalizeRollCallLedger({
      active: false,
      rounds_total: 3,
      window_ms: 135_000,
      min_interval_ms: 300_000,
      max_text_bytes: 48,
      pending_dropped: 0,
      answer_limited: 0,
    });

    expect(led.active).toBe(false);
    expect(led.rollcallId).toBeUndefined();
    expect(led.responders).toEqual([]);
    expect(led.missing).toEqual([]);
    // The caps are still readable, so a client can build its form before any
    // roll-call exists.
    expect(led.maxTextBytes).toBe(48);
  });

  it('drops a row whose address cannot be read rather than showing node zero', () => {
    const led = normalizeRollCallLedger({
      active: true,
      responders: [
        { address: 'nonsense', responded: true },
        { address: '0000000B', responded: true },
      ],
      missing: ['also nonsense', '0000000D'],
    });

    expect(led.responders.map(r => r.addr)).toEqual([0x0000000b]);
    expect(led.missing).toEqual([0x0000000d]);
  });

  it('omits the relay path when the receipt machinery supplied none', () => {
    const led = normalizeRollCallLedger({
      active: true,
      responders: [{ address: '0000000B', responded: true, at_ms: 100, round: 1 }],
    });

    expect(led.responders[0].relayPath).toBeUndefined();
  });
});
