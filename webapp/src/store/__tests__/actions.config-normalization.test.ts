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

  it('falls back to alias name only when primary name is blank', () => {
    const cfg = normalizeConfig({
      channels: [
        {
          id: 2,
          name: '   ',
          channel_name: 'ops-net',
        },
        {
          id: 3,
          name: 'control',
          channel_name: 'ignored-alias',
        },
      ],
    });

    expect(cfg.channels[0].name).toBe('ops-net');
    expect(cfg.channels[1].name).toBe('control');
  });
});
