import { describe, it, expect, vi, afterEach } from 'vitest';
import { describePlatform } from '../platform';

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

function navStub(ua: string, extra: Partial<Navigator> = {}): Navigator {
  return { userAgent: ua, maxTouchPoints: 0, ...extra } as Navigator;
}

const DESKTOP_CHROME =
  'Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0 Safari/537.36';
const ANDROID_CHROME =
  'Mozilla/5.0 (Linux; Android 14) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0 Mobile Safari/537.36';
const IPHONE_SAFARI =
  'Mozilla/5.0 (iPhone; CPU iPhone OS 17_5 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.5 Mobile/15E148 Safari/604.1';
const IPADOS_SAFARI =
  'Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.5 Safari/605.1.15';
const MAC_SAFARI = IPADOS_SAFARI;

describe('describePlatform', () => {
  it('reports desktop for a desktop user agent', () => {
    expect(describePlatform(navStub(DESKTOP_CHROME))).toBe('desktop');
  });

  it('reports android for Chrome on Android', () => {
    expect(describePlatform(navStub(ANDROID_CHROME))).toBe('android');
  });

  it('reports ios for an iPhone', () => {
    expect(describePlatform(navStub(IPHONE_SAFARI))).toBe('ios');
  });

  it('reports ios for iPadOS, which claims a Macintosh user agent', () => {
    expect(describePlatform(navStub(IPADOS_SAFARI, { maxTouchPoints: 5 }))).toBe('ios');
  });

  it('reports desktop for a real Mac, which has no touch points', () => {
    expect(describePlatform(navStub(MAC_SAFARI, { maxTouchPoints: 0 }))).toBe('desktop');
  });

  it('falls back to userAgentData.mobile for an unrecognised mobile browser', () => {
    const nav = navStub('Mozilla/5.0 (Unknown)') as Navigator & { userAgentData?: { mobile: boolean } };
    nav.userAgentData = { mobile: true };
    expect(describePlatform(nav)).toBe('android');
  });
});
