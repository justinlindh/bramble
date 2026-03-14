import { describe, it, expect, vi, afterEach } from 'vitest';

describe('platform detection', () => {
  afterEach(() => {
    vi.unstubAllGlobals();
  });

  it('detects electron environment', async () => {
    vi.stubGlobal('isElectron', true);
    const { isElectron } = await import('../platform');
    expect(isElectron()).toBe(true);
  });

  it('detects browser environment', async () => {
    vi.stubGlobal('isElectron', undefined);
    vi.resetModules();
    const { isElectron } = await import('../platform');
    expect(isElectron()).toBe(false);
  });

  it('detects serial support', async () => {
    vi.stubGlobal('navigator', { serial: {} });
    vi.resetModules();
    const { hasSerialSupport } = await import('../platform');
    expect(hasSerialSupport()).toBe(true);
  });

  it('detects missing serial support', async () => {
    vi.stubGlobal('navigator', {});
    vi.resetModules();
    const { hasSerialSupport } = await import('../platform');
    expect(hasSerialSupport()).toBe(false);
  });

  it('detects BLE support', async () => {
    vi.stubGlobal('navigator', { bluetooth: {} });
    vi.resetModules();
    const { hasBLESupport } = await import('../platform');
    expect(hasBLESupport()).toBe(true);
  });
});
