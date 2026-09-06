import { describe, it, expect } from 'vitest';
import { compareSemver } from '../semver';

describe('compareSemver', () => {
  it('orders by core version', () => {
    expect(compareSemver('1.4.0', '1.5.0')).toBeLessThan(0);
    expect(compareSemver('2.0.0', '1.9.9')).toBeGreaterThan(0);
    expect(compareSemver('1.4.0', '1.4.0')).toBe(0);
  });

  it('orders core identifiers numerically, not lexically', () => {
    expect(compareSemver('v1.10.0', 'v1.9.0')).toBeGreaterThan(0);
  });

  it('treats a prerelease as older than its release', () => {
    expect(compareSemver('1.5.0-rc.1', '1.5.0')).toBeLessThan(0);
    expect(compareSemver('1.5.0', '1.5.0-rc.1')).toBeGreaterThan(0);
    expect(compareSemver('1.5.0-rc.1', '1.5.0-rc.2')).toBeLessThan(0);
    expect(compareSemver('1.4.0-rc1', '1.4.0')).toBeLessThan(0);
  });

  it('compares numeric prerelease identifiers numerically, not lexically', () => {
    expect(compareSemver('1.4.0-rc.10', '1.4.0-rc.9')).toBeGreaterThan(0);
    expect(compareSemver('1.4.0-rc.2', '1.4.0-rc.10')).toBeLessThan(0);
  });

  it('ranks a shorter prerelease lower when the shared identifiers are equal', () => {
    expect(compareSemver('1.4.0-alpha', '1.4.0-alpha.1')).toBeLessThan(0);
  });

  it('ranks numeric identifiers below alphanumeric identifiers', () => {
    expect(compareSemver('1.4.0-1', '1.4.0-alpha')).toBeLessThan(0);
  });

  it('ranks a full release above the local dev version string', () => {
    expect(compareSemver('v1.4.0', '0.0.0-local')).toBeGreaterThan(0);
  });

  it('tolerates a leading v', () => {
    expect(compareSemver('v1.4.0', '1.5.0')).toBeLessThan(0);
  });
});
