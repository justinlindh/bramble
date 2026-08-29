import { describe, it, expect } from 'vitest';
import { compareSemver } from '../semver';

describe('compareSemver', () => {
  it('orders by core version', () => {
    expect(compareSemver('1.4.0', '1.5.0')).toBeLessThan(0);
    expect(compareSemver('2.0.0', '1.9.9')).toBeGreaterThan(0);
    expect(compareSemver('1.4.0', '1.4.0')).toBe(0);
  });

  it('treats a prerelease as older than its release', () => {
    expect(compareSemver('1.5.0-rc.1', '1.5.0')).toBeLessThan(0);
    expect(compareSemver('1.5.0', '1.5.0-rc.1')).toBeGreaterThan(0);
    expect(compareSemver('1.5.0-rc.1', '1.5.0-rc.2')).toBeLessThan(0);
  });

  it('tolerates a leading v', () => {
    expect(compareSemver('v1.4.0', '1.5.0')).toBeLessThan(0);
  });
});
