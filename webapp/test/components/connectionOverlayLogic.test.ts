import { describe, it, expect } from 'vitest';
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

  it('never embeds the token in the URL (moved to WS subprotocol)', () => {
    expect(buildWifiUrl('192.168.4.1', 'http:', 'app.local', 'abc 123'))
      .toBe('ws://192.168.4.1/ws');
  });

  it('leaves an explicit ws:// URL untouched and adds no token', () => {
    expect(buildWifiUrl('ws://10.0.0.5/ws?foo=bar', 'http:', 'app.local', 'tok'))
      .toBe('ws://10.0.0.5/ws?foo=bar');
  });
});
