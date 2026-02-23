import { describe, expect, it } from 'vitest';
import { normalizeConfig } from '../actions';

describe('normalizeConfig channel metadata aliases', () => {
  it('maps mixed firmware key variants for name, hasPsk, epoch, and default', () => {
    const cfg = normalizeConfig({
      channels: [
        {
          id: 3,
          channel_name: 'ops-net',
          has_psk: true,
          key_epoch: 7,
          default_channel: true,
        },
        {
          index: 4,
          channelName: 'field-team',
          pskEnabled: false,
          keyEpoch: 9,
          defaultChannel: false,
        },
      ],
    });

    expect(cfg.channels).toEqual([
      {
        index: 3,
        name: 'ops-net',
        hasPsk: true,
        epoch: 7,
        isDefault: true,
      },
      {
        index: 4,
        name: 'field-team',
        hasPsk: false,
        epoch: 9,
        isDefault: false,
      },
    ]);
  });
});
