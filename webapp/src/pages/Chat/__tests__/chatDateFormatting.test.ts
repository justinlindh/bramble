import { describe, expect, it, vi, beforeEach, afterEach } from 'vitest';
import { formatDaySeparatorLabel, formatMessageTimestamp, shouldInsertDaySeparator } from '../chatDateFormatting';

describe('chatDateFormatting', () => {
  const now = new Date('2026-03-05T23:58:00-08:00');

  beforeEach(() => {
    vi.useFakeTimers();
    vi.setSystemTime(now);
  });

  afterEach(() => {
    vi.useRealTimers();
  });

  it('shows only time for messages from today', () => {
    const messageTs = new Date('2026-03-05T22:32:00-08:00').getTime();

    expect(formatMessageTimestamp(messageTs)).toBe('10:32 PM');
  });

  it('shows Yesterday + time for messages from yesterday', () => {
    const messageTs = new Date('2026-03-04T22:32:00-08:00').getTime();

    expect(formatMessageTimestamp(messageTs)).toBe('Yesterday 10:32 PM');
  });

  it('shows month/day + time for older messages', () => {
    const messageTs = new Date('2026-03-03T22:32:00-08:00').getTime();

    expect(formatMessageTimestamp(messageTs)).toBe('Mar 3, 10:32 PM');
  });

  it('formats day separator label with full date', () => {
    const messageTs = new Date('2026-03-03T14:00:00-08:00').getTime();

    expect(formatDaySeparatorLabel(messageTs)).toBe('March 3, 2026');
  });

  it('inserts separator when day changes', () => {
    const first = new Date('2026-03-04T22:00:00-08:00').getTime();
    const second = new Date('2026-03-03T23:00:00-08:00').getTime();

    expect(shouldInsertDaySeparator(undefined, first)).toBe(true);
    expect(shouldInsertDaySeparator(first, first + 60_000)).toBe(false);
    expect(shouldInsertDaySeparator(first, second)).toBe(true);
  });
});
