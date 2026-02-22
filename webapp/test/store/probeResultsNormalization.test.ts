import { describe, it, expect } from 'vitest';
import { upsertProbeResponse } from '../../src/store/actions';
import type { ProbeResponse } from '../../src/types/bramble';

describe('probe results normalization', () => {
  it('adds a new responder row when unseen', () => {
    const rows: ProbeResponse[] = [];
    const out = upsertProbeResponse(rows, {
      responderAddr: 0x1111,
      hopCount: 1,
      rssi: -70,
      snr: 8,
      pathLen: 1,
      latencyMs: 500,
      receivedAt: 1000,
    });

    expect(out).toHaveLength(1);
    expect(out[0].responderAddr).toBe(0x1111);
  });

  it('dedupes by responder and keeps best quality + latest latency', () => {
    const rows: ProbeResponse[] = [{
      responderAddr: 0x2222,
      hopCount: 1,
      rssi: -90,
      snr: 4,
      pathLen: 1,
      latencyMs: 400,
      receivedAt: 1000,
    }];

    const out = upsertProbeResponse(rows, {
      responderAddr: 0x2222,
      hopCount: 2,
      rssi: -60,
      snr: 11,
      pathLen: 2,
      latencyMs: 750,
      receivedAt: 1500,
    });

    expect(out).toHaveLength(1);
    expect(out[0].rssi).toBe(-60);
    expect(out[0].snr).toBe(11);
    expect(out[0].latencyMs).toBe(750);
    expect(out[0].receivedAt).toBe(1500);
    expect(out[0].hopCount).toBe(2);
  });
});
