import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { renderHook, act } from '@testing-library/react';
import { useTimedFlag } from '../useTimedFlag';

beforeEach(() => vi.useFakeTimers());
afterEach(() => vi.useRealTimers());

describe('useTimedFlag', () => {
  it('sets the flag on trigger and clears it after the delay', () => {
    const { result } = renderHook(() => useTimedFlag(1000));
    expect(result.current[0]).toBe(false);

    act(() => result.current[1]());
    expect(result.current[0]).toBe(true);

    act(() => vi.advanceTimersByTime(999));
    expect(result.current[0]).toBe(true);

    act(() => vi.advanceTimersByTime(1));
    expect(result.current[0]).toBe(false);
  });

  it('restarts the window when triggered again mid-flash', () => {
    const { result } = renderHook(() => useTimedFlag(1000));
    act(() => result.current[1]());
    act(() => vi.advanceTimersByTime(800));
    act(() => result.current[1]());
    act(() => vi.advanceTimersByTime(800));
    expect(result.current[0]).toBe(true); // would have expired without the restart
    act(() => vi.advanceTimersByTime(200));
    expect(result.current[0]).toBe(false);
  });

  it('reset clears the flag immediately', () => {
    const { result } = renderHook(() => useTimedFlag(1000));
    act(() => result.current[1]());
    expect(result.current[0]).toBe(true);
    act(() => result.current[2]());
    expect(result.current[0]).toBe(false);
  });

  it('cancels the pending timer on unmount (no setState after unmount)', () => {
    const { result, unmount } = renderHook(() => useTimedFlag(1000));
    act(() => result.current[1]());
    unmount();
    // Advancing past the delay must not throw or warn: the timer was cancelled.
    expect(() => act(() => vi.advanceTimersByTime(2000))).not.toThrow();
  });
});
