import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { renderHook } from '@testing-library/react';
import { usePoll } from '../../src/hooks/usePoll';

describe('usePoll', () => {
  beforeEach(() => {
    vi.useFakeTimers();
  });

  afterEach(() => {
    vi.useRealTimers();
  });

  it('calls fn immediately and then on every interval by default', async () => {
    const fn = vi.fn();
    renderHook(() => usePoll(fn, 1000));

    await vi.advanceTimersByTimeAsync(0);
    expect(fn).toHaveBeenCalledTimes(1);

    await vi.advanceTimersByTimeAsync(1000);
    expect(fn).toHaveBeenCalledTimes(2);

    await vi.advanceTimersByTimeAsync(2000);
    expect(fn).toHaveBeenCalledTimes(4);
  });

  it('skips ticks while enabled is false but keeps the schedule alive', async () => {
    const fn = vi.fn();
    const { rerender } = renderHook(
      ({ enabled }: { enabled: boolean }) => usePoll(fn, 1000, { enabled }),
      { initialProps: { enabled: false } },
    );

    // Immediate tick and several intervals pass without a single invocation.
    await vi.advanceTimersByTimeAsync(3000);
    expect(fn).not.toHaveBeenCalled();

    // Flipping enabled does not restart the effect (same interval identity);
    // the already-scheduled next tick picks the new value up.
    rerender({ enabled: true });
    expect(fn).not.toHaveBeenCalled();
    await vi.advanceTimersByTimeAsync(1000);
    expect(fn).toHaveBeenCalledTimes(1);
  });

  it('stops invoking fn on the tick after enabled flips back to false', async () => {
    const fn = vi.fn();
    const { rerender } = renderHook(
      ({ enabled }: { enabled: boolean }) => usePoll(fn, 1000, { enabled }),
      { initialProps: { enabled: true } },
    );

    await vi.advanceTimersByTimeAsync(0);
    expect(fn).toHaveBeenCalledTimes(1);

    rerender({ enabled: false });
    await vi.advanceTimersByTimeAsync(5000);
    expect(fn).toHaveBeenCalledTimes(1);
  });

  it('keeps polling when fn rejects', async () => {
    const fn = vi.fn().mockRejectedValue(new Error('poll failed'));
    renderHook(() => usePoll(fn, 1000));

    await vi.advanceTimersByTimeAsync(0);
    expect(fn).toHaveBeenCalledTimes(1);

    await vi.advanceTimersByTimeAsync(1000);
    expect(fn).toHaveBeenCalledTimes(2);
  });

  it('stops polling on unmount', async () => {
    const fn = vi.fn();
    const { unmount } = renderHook(() => usePoll(fn, 1000));

    await vi.advanceTimersByTimeAsync(0);
    expect(fn).toHaveBeenCalledTimes(1);

    unmount();
    await vi.advanceTimersByTimeAsync(5000);
    expect(fn).toHaveBeenCalledTimes(1);
  });
});
