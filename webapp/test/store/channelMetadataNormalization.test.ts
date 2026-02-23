import { describe, it, expect } from 'vitest';
import { normalizeConfig } from '../../src/store/actions';

describe('channel metadata normalization', () => {
  it('preserves explicit channel name and maps snake_case PSK/default flags', () => {
    const normalized = normalizeConfig({
      address: '00000001',
      pubkey_hash: '00000002',
      node_name: 'node',
      radio: {},
      channels: [
        {
          id: 3,
          name: 'ops-room',
          has_psk: true,
          is_default: false,
          epoch: 7,
        },
      ],
    });

    expect(normalized.channels).toHaveLength(1);
    expect(normalized.channels[0]).toMatchObject({
      index: 3,
      name: 'ops-room',
      hasPsk: true,
      isDefault: false,
      epoch: 7,
    });
  });

  it('accepts default alias for default-channel flag', () => {
    const normalized = normalizeConfig({
      address: '00000001',
      pubkey_hash: '00000002',
      node_name: 'node',
      radio: {},
      channels: [
        {
          id: 1,
          name: 'team',
          hasPsk: true,
          default: true,
        },
      ],
    });

    expect(normalized.channels[0].isDefault).toBe(true);
  });
});
