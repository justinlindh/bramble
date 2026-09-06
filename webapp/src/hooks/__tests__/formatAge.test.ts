import { describe, it, expect } from 'vitest';
import { formatAge } from '../useAgeTick';

describe('formatAge', () => {
  it('reads sub-second ages as "just now"', () => {
    expect(formatAge(0)).toBe('just now');
    expect(formatAge(500)).toBe('just now');
    expect(formatAge(999)).toBe('just now');
  });

  it('reports whole seconds up to a minute', () => {
    expect(formatAge(1000)).toBe('1s ago');
    expect(formatAge(5000)).toBe('5s ago');
    expect(formatAge(59_000)).toBe('59s ago');
  });

  it('rolls over to minutes at sixty seconds', () => {
    expect(formatAge(60_000)).toBe('1m ago');
    expect(formatAge(90_000)).toBe('1m ago'); // floors, does not round up
    expect(formatAge(59 * 60_000)).toBe('59m ago');
  });

  it('rolls over to hours at sixty minutes', () => {
    expect(formatAge(60 * 60_000)).toBe('1h ago');
    expect(formatAge(2 * 60 * 60_000)).toBe('2h ago');
  });
});
