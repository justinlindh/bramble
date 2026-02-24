import { describe, it, expect } from 'vitest';
import { normalizeConfig } from '../../src/store/actions';

describe('normalizeConfig location policy defaults', () => {
  it('uses privacy-first defaults when location config is missing', () => {
    const cfg = normalizeConfig({ address: '00000001', pubkey_hash: '00000001', node_name: 'node', channels: [] });

    expect(cfg.location.enabled).toBe(false);
    expect(cfg.location.default_tier).toBe('coarse');
    expect(cfg.location.interval_s).toBe(300);
    expect(cfg.location.source).toBe('hybrid');
    expect(cfg.location.contact_rules).toEqual([]);
    expect(cfg.location.channel_targets).toEqual([]);
  });

  it('maps legacy contacts to contact_rules', () => {
    const cfg = normalizeConfig({
      address: '00000001',
      pubkey_hash: '00000001',
      node_name: 'node',
      channels: [],
      location: {
        enabled: true,
        contacts: [
          { addr: 0x1234abcd, tier: 'full', intervalSec: 180, distanceTriggerM: 50 },
        ],
      },
    });

    expect(cfg.location.contact_rules).toEqual([
      { address: '1234ABCD', enabled: true, tier: 'full', interval_s: 180 },
    ]);
  });
});
