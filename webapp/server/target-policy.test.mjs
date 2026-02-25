// @vitest-environment node

import { describe, expect, it } from 'vitest';
import { isAllowedTarget, parseAllowlist, splitTarget } from './target-policy.mjs';

describe('target policy', () => {
  const cfg = { mode: 'local', allowlist: [] };

  it('allows RFC1918 IP in local mode', () => {
    expect(isAllowedTarget('192.0.2.0', cfg)).toBe(true);
  });

  it('rejects public IP', () => {
    expect(isAllowedTarget('8.8.8.8', cfg)).toBe(false);
  });

  it('rejects hostname by default', () => {
    expect(isAllowedTarget('device.local', cfg)).toBe(false);
  });

  it('allows public target in explicit allowlist CIDR', () => {
    const allowlist = parseAllowlist('8.8.8.0/24,10.1.0.0/16');
    expect(isAllowedTarget('8.8.8.8', { mode: 'local', allowlist })).toBe(true);
  });

  it('parses host:port and rejects invalid port values', () => {
    expect(splitTarget('192.0.2.0')).toEqual({ host: '192.0.2.0', port: null });
    expect(splitTarget('192.0.2.0:3005')).toEqual({ host: '192.0.2.0', port: 3005 });
    expect(splitTarget('192.0.2.0:70000')).toBeNull();
  });
});
