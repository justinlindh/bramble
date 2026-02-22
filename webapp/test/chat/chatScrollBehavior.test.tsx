import { describe, it, expect } from 'vitest';
import { isNearBottom } from '../../src/pages/Chat/Chat';

describe('chat scroll behavior helpers', () => {
  it('returns true when user is near bottom within threshold', () => {
    expect(isNearBottom({ scrollTop: 790, clientHeight: 200, scrollHeight: 1000 }, 20)).toBe(true);
  });

  it('returns false when user is far from bottom', () => {
    expect(isNearBottom({ scrollTop: 400, clientHeight: 200, scrollHeight: 1000 }, 20)).toBe(false);
  });
});
