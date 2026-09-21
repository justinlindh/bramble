import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { renderHook, act } from '@testing-library/react';
import { useTimedValue } from '../useTimedValue';

beforeEach(() => vi.useFakeTimers());
afterEach(() => vi.useRealTimers());

describe('useTimedValue', () => {
  it('shows the value and reverts to the cleared value after the delay', () => {
    const { result } = renderHook(() => useTimedValue('', 1000));
    expect(result.current[0]).toBe('');

    act(() => result.current[1]('saved'));
    expect(result.current[0]).toBe('saved');

    act(() => vi.advanceTimersByTime(999));
    expect(result.current[0]).toBe('saved');

    act(() => vi.advanceTimersByTime(1));
    expect(result.current[0]).toBe('');
  });

  it('restarts the window when shown again mid-window', () => {
    const { result } = renderHook(() => useTimedValue('', 1000));
    act(() => result.current[1]('first'));
    act(() => vi.advanceTimersByTime(800));
    act(() => result.current[1]('second'));
    expect(result.current[0]).toBe('second');
    act(() => vi.advanceTimersByTime(800));
    expect(result.current[0]).toBe('second'); // would have expired without the restart
    act(() => vi.advanceTimersByTime(200));
    expect(result.current[0]).toBe('');
  });

  it('reset clears the value immediately', () => {
    const { result } = renderHook(() => useTimedValue('', 1000));
    act(() => result.current[1]('saved'));
    expect(result.current[0]).toBe('saved');
    act(() => result.current[2]());
    expect(result.current[0]).toBe('');
  });

  it('supports a non-string cleared value', () => {
    const { result } = renderHook(() => useTimedValue<string | null>(null, 1000));
    expect(result.current[0]).toBe(null);
    act(() => result.current[1]('label'));
    expect(result.current[0]).toBe('label');
    act(() => vi.advanceTimersByTime(1000));
    expect(result.current[0]).toBe(null);
  });

  it('cancels the pending timer on unmount (no setState after unmount)', () => {
    const { result, unmount } = renderHook(() => useTimedValue('', 1000));
    act(() => result.current[1]('saved'));
    unmount();
    // Advancing past the delay must not throw or warn: the timer was cancelled.
    expect(() => act(() => vi.advanceTimersByTime(2000))).not.toThrow();
  });
});
