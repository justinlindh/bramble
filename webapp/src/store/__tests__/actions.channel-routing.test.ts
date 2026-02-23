import { describe, expect, it } from 'vitest';
import { normalizeIncomingRealtimeMessage } from '../actions';

describe('normalizeIncomingRealtimeMessage', () => {
  it('keeps channel-scoped messages in their channel even when broadcast flag is set', () => {
    const msg = normalizeIncomingRealtimeMessage({
      from: 'A1B2C3D4',
      text: 'hello',
      broadcast: true,
      channel: 2,
    });

    expect(msg.channelIndex).toBe(2);
    expect(msg.to).toBe(0);
  });

  it('maps broadcast notifications without explicit to address to broadcast destination', () => {
    const msg = normalizeIncomingRealtimeMessage({
      from: 'A1B2C3D4',
      text: 'public hello',
      broadcast: true,
      channel: -1,
    });

    expect(msg.channelIndex).toBeUndefined();
    expect(msg.to).toBe(0xffffffff);
  });
});
