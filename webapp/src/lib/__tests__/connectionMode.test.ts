import { describe, it, expect, vi, afterEach } from 'vitest';
import { fetchConnectionCapabilities, DEFAULT_CAPABILITIES, ELECTRON_CAPABILITIES } from '../connectionMode';

describe('fetchConnectionCapabilities', () => {
  afterEach(() => {
    vi.unstubAllGlobals();
  });

  it('short-circuits to local capabilities in Electron without fetching', async () => {
    vi.stubGlobal('isElectron', true);
    const fetchSpy = vi.fn();
    const caps = await fetchConnectionCapabilities(fetchSpy as unknown as typeof fetch);
    expect(caps).toEqual(ELECTRON_CAPABILITIES);
    expect(caps.mode).toBe('local');
    expect(caps.localLanAllowed).toBe(true);
    expect(fetchSpy).not.toHaveBeenCalled();
  });

  it('falls back to the capabilities fetch outside Electron', async () => {
    vi.stubGlobal('isElectron', undefined);
    const fetchSpy = vi.fn().mockRejectedValue(new Error('no server'));
    const caps = await fetchConnectionCapabilities(fetchSpy as unknown as typeof fetch);
    expect(caps).toEqual(DEFAULT_CAPABILITIES);
    expect(fetchSpy).toHaveBeenCalledTimes(1);
  });
});
