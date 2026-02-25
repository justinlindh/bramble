// @vitest-environment node

import { describe, expect, it } from 'vitest';
import { isAllowedTarget } from './target-policy.mjs';

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
});
