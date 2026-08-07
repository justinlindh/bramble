import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { renderHook, act } from '@testing-library/react';

const copyWithFallback = vi.fn<(text: string) => Promise<boolean>>();
vi.mock('../../utils/clipboard', () => ({
  copyWithFallback: (text: string) => copyWithFallback(text),
}));

import { useCopyFlash } from '../useCopyFlash';

beforeEach(() => {
  vi.useFakeTimers();
  copyWithFallback.mockReset();
});
afterEach(() => vi.useRealTimers());

describe('useCopyFlash', () => {
  it('flashes and resolves true when the copy succeeds', async () => {
    copyWithFallback.mockResolvedValue(true);
    const { result } = renderHook(() => useCopyFlash(1000));
    expect(result.current[0]).toBe(false);

    let ok: boolean | undefined;
    await act(async () => {
      ok = await result.current[1]('hello');
    });
    expect(ok).toBe(true);
    expect(copyWithFallback).toHaveBeenCalledWith('hello');
    expect(result.current[0]).toBe(true);

    act(() => vi.advanceTimersByTime(1000));
    expect(result.current[0]).toBe(false);
  });

  it('does not flash and resolves false when the copy fails', async () => {
    copyWithFallback.mockResolvedValue(false);
    const { result } = renderHook(() => useCopyFlash(1000));

    let ok: boolean | undefined;
    await act(async () => {
      ok = await result.current[1]('hello');
    });
    expect(ok).toBe(false);
    expect(result.current[0]).toBe(false);
  });

  it('reset clears the indicator immediately', async () => {
    copyWithFallback.mockResolvedValue(true);
    const { result } = renderHook(() => useCopyFlash(1000));
    await act(async () => {
      await result.current[1]('hello');
    });
    expect(result.current[0]).toBe(true);
    act(() => result.current[2]());
    expect(result.current[0]).toBe(false);
  });
});
