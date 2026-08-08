import { describe, expect, it } from 'vitest';
import { mergeFirmwareMessages } from '../actions/messaging';

const baseCtx = { existing: [], deviceUptime: 0, myAddr: 0, now: 1000 };

describe('mergeFirmwareMessages status mapping', () => {
  it('maps a parked (queued) firmware row to the parked DeliveryStatus', () => {
    const [msg] = mergeFirmwareMessages(
      [
        {
          from: '00000001',
          to: '00000002',
          direction: 'outgoing',
          text: 'hi',
          channel: -1,
          broadcast: false,
          timestamp_s: 1,
          status: 'queued',
        },
      ],
      baseCtx,
    );
    expect(msg.status).toBe('parked');
  });

  it.each(['sent', 'delivered', 'failed', undefined] as const)(
    'collapses firmware status %s to delivered, matching the pre-existing simplification',
    (status) => {
      const [msg] = mergeFirmwareMessages(
        [
          {
            from: '00000001',
            to: '00000002',
            direction: 'outgoing',
            text: 'hi',
            channel: -1,
            broadcast: false,
            timestamp_s: 1,
            status,
          },
        ],
        baseCtx,
      );
      expect(msg.status).toBe('delivered');
    },
  );
});
