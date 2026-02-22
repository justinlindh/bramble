import { describe, it, expect } from 'vitest';
import { getEmptyHint, isNearBottom } from '../../src/pages/Chat/Chat';

describe('Chat helpers', () => {
  it('returns the correct empty hint by conversation type', () => {
    expect(getEmptyHint('broadcast')).toContain('Broadcast messages');
    expect(getEmptyHint('ch:2')).toContain('Channel messages');
    expect(getEmptyHint('dm:123')).toContain('Send a message');
  });

  it('detects near-bottom thresholds', () => {
    expect(isNearBottom({ scrollTop: 880, clientHeight: 100, scrollHeight: 1000 }, 20)).toBe(true);
    expect(isNearBottom({ scrollTop: 700, clientHeight: 100, scrollHeight: 1000 }, 20)).toBe(false);
  });
});
