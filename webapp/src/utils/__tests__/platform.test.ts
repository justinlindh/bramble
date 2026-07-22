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

  it('detects the Android shell environment', async () => {
    vi.stubGlobal('brambleAndroid', true);
    vi.resetModules();
    const { isAndroidShell, isEmbeddedShell } = await import('../platform');
    expect(isAndroidShell()).toBe(true);
    expect(isEmbeddedShell()).toBe(true);
  });

  it('treats electron as an embedded shell', async () => {
    vi.stubGlobal('isElectron', true);
    vi.stubGlobal('brambleAndroid', undefined);
    vi.resetModules();
    const { isAndroidShell, isEmbeddedShell } = await import('../platform');
    expect(isAndroidShell()).toBe(false);
    expect(isEmbeddedShell()).toBe(true);
  });

  it('is not an embedded shell in a plain browser', async () => {
    vi.stubGlobal('isElectron', undefined);
    vi.stubGlobal('brambleAndroid', undefined);
    vi.resetModules();
    const { isEmbeddedShell } = await import('../platform');
    expect(isEmbeddedShell()).toBe(false);
  });
});
