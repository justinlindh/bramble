import { describe, it, expect } from 'vitest';
import { buildWifiUrl, shouldAutoConnect } from '../../src/components/ConnectionOverlay';

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

  it('appends token query param when provided', () => {
    expect(buildWifiUrl('192.168.4.1', 'http:', 'localhost:4173', 'abc 123'))
      .toBe('ws://192.168.4.1/ws?token=abc%20123');
  });

  it('appends token with ampersand when URL already has query params', () => {
    expect(buildWifiUrl('ws://10.0.0.5/ws?foo=bar', 'http:', 'localhost:4173', 'tok'))
      .toBe('ws://10.0.0.5/ws?foo=bar&token=tok');
  });

  it('never auto-connects; user must click Connect', () => {
    expect(shouldAutoConnect('192.168.4.1', false, 'disconnected', false)).toBe(false);
    expect(shouldAutoConnect('', false, 'disconnected', false)).toBe(false);
    expect(shouldAutoConnect('192.168.4.1', true, 'disconnected', false)).toBe(false);
    expect(shouldAutoConnect('192.168.4.1', false, 'connecting', false)).toBe(false);
    expect(shouldAutoConnect('192.168.4.1', false, 'disconnected', true)).toBe(false);
  });
});
