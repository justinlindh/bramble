import { describe, expect, it } from 'vitest';
import { countEnabledShareTargets } from '../locationSharing';

describe('countEnabledShareTargets', () => {
  it('sums enabled contacts and channels', () => {
    expect(
      countEnabledShareTargets(
        [{ enabled: true }, { enabled: true }],
        [{ enabled: true }],
      ),
    ).toBe(3);
  });

  it('excludes only targets explicitly disabled', () => {
    expect(
      countEnabledShareTargets(
        [{ enabled: true }, { enabled: false }],
        [{ enabled: false }],
      ),
    ).toBe(1);
  });

  it('counts a target that omits enabled, since only explicit false opts out', () => {
    expect(countEnabledShareTargets([{}], [{}])).toBe(2);
  });

  it('is zero for empty target lists', () => {
    expect(countEnabledShareTargets([], [])).toBe(0);
  });
});
