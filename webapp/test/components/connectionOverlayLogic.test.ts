import { describe, it, expect, vi } from 'vitest';
import { buildWifiUrl } from '../../src/components/ConnectionOverlay';

describe('ConnectionOverlay URL logic', () => {
  it('keeps full URL inputs unchanged', () => {
    expect(buildWifiUrl('ws://10.0.0.5/ws', 'http:', 'localhost:4173')).toBe('ws://10.0.0.5/ws');
  });

  it('uses proxy URL on https pages', () => {
    expect(buildWifiUrl('192.168.4.1', 'https:', 'app.example.com')).toBe('wss://app.example.com/proxy/192.168.4.1');
  });

  it('uses direct ws URL on http pages', () => {
    expect(buildWifiUrl('192.168.4.1', 'http:', 'localhost:4173')).toBe('ws://192.168.4.1/ws');
  });

  it('leaves an explicit ws:// URL with a query string untouched', () => {
    expect(buildWifiUrl('ws://10.0.0.5/ws?foo=bar', 'http:', 'app.local'))
      .toBe('ws://10.0.0.5/ws?foo=bar');
  });

  it('uses direct ws URL in an embedded shell even on an https origin', () => {
    vi.stubGlobal('brambleAndroid', true);
    try {
      expect(buildWifiUrl('192.168.4.1', 'https:', 'appassets.androidplatform.net'))
        .toBe('ws://192.168.4.1/ws');
    } finally {
      vi.unstubAllGlobals();
    }
  });
});
