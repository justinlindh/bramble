import { describe, it, expect } from 'vitest';
import { formatAge } from '../../src/hooks/useAgeTick';

describe('formatAge', () => {
  it('returns "just now" for elapsed time under one second', () => {
    expect(formatAge(0)).toBe('just now');
    expect(formatAge(999)).toBe('just now');
  });

  it('returns seconds for elapsed time under one minute', () => {
    expect(formatAge(1000)).toBe('1s ago');
    expect(formatAge(5000)).toBe('5s ago');
    expect(formatAge(59_999)).toBe('59s ago');
  });

  it('returns minutes for elapsed time between one and sixty minutes', () => {
    expect(formatAge(60_000)).toBe('1m ago');
    expect(formatAge(90_000)).toBe('1m ago');
    expect(formatAge(120_000)).toBe('2m ago');
    expect(formatAge(3599_000)).toBe('59m ago');
  });

  it('returns hours for elapsed time of one hour or more', () => {
    expect(formatAge(3_600_000)).toBe('1h ago');
    expect(formatAge(7_200_000)).toBe('2h ago');
    expect(formatAge(10_800_000)).toBe('3h ago');
  });
});
