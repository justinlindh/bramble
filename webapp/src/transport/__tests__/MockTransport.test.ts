import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { MockTransport } from '../MockTransport';

// Drives the real mock/handler.mjs RPC logic in process (no network), exactly
// as the in-page mock does for embedded shells (Android WebView, Electron
// under file://). We assert against the real handler's output, not a stub.
describe('MockTransport (in-page mock for embedded shells)', () => {
  beforeEach(() => {
    vi.useFakeTimers();
  });

  afterEach(() => {
    vi.useRealTimers();
  });

  it('connect() resolves immediately, no network involved', async () => {
    const transport = new MockTransport();
    expect(transport.connected).toBe(false);
    await transport.connect();
    expect(transport.connected).toBe(true);
  });

  it('round-trips a real RPC through the mock handler (bramble.getConfig)', async () => {
    const transport = new MockTransport();
    await transport.connect();

    const config = await transport.sendRPC<{
      identity: { name: string; address: number };
      radio: { sf: number };
    }>('bramble.getConfig');

    expect(config.identity).toBeTruthy();
    expect(typeof config.identity.name).toBe('string');
    expect(typeof config.identity.address).toBe('number');
    expect(typeof config.radio.sf).toBe('number');
  });

  it('surfaces an RPC error from the handler (unknown method)', async () => {
    const transport = new MockTransport();
    await transport.connect();
    await expect(transport.sendRPC('bramble.notARealMethod')).rejects.toThrow(/Method not found/);
  });

  it('delivers a notification pushed by the handler to onNotification', async () => {
    const transport = new MockTransport();
    await transport.connect();

    const received: { method: string; params: unknown }[] = [];
    transport.onNotification((method, params) => received.push({ method, params }));

    // bramble.shareLocationOnce replies synchronously, then notify()s a
    // location.update 500ms later. A real handler code path, not a fake.
    const ack = await transport.sendRPC<{ ok: boolean }>('bramble.shareLocationOnce', { address: 'AABBCC01' });
    expect(ack.ok).toBe(true);
    expect(received).toHaveLength(0);

    await vi.advanceTimersByTimeAsync(500);

    expect(received).toHaveLength(1);
    expect(received[0].method).toBe('location.update');
    expect(received[0].params).toMatchObject({ addr: 'AABBCC01' });
  });

  it('rejects the call when dispatch into the handler throws', async () => {
    const transport = new MockTransport();
    await transport.connect();

    (transport as unknown as { messageListener: (data: string) => void }).messageListener = () => {
      throw new Error('handler exploded');
    };

    await expect(transport.sendRPC('bramble.getConfig')).rejects.toThrow(
      'Mock RPC dispatch failed: handler exploded'
    );
  });

  it('disconnect() tears down the fake socket and future RPCs fail', async () => {
    const transport = new MockTransport();
    await transport.connect();
    await transport.disconnect();

    expect(transport.connected).toBe(false);
    await expect(transport.sendRPC('bramble.getConfig')).rejects.toThrow(/Not connected/);
  });

  describe('OTA journey simulation', () => {
    it('otaGetOrigin/otaSetOrigin manage the mock OTA origin state', async () => {
      const transport = new MockTransport();
      await transport.connect();

      // The mock's OTA origin is same-origin (<page origin>/ota/) so
      // fetchOtaIndex can actually resolve it in the browser/dev-server;
      // jsdom's default test origin is http://localhost:3000.
      const initial = await transport.sendRPC<any>('bramble.otaGetOrigin');
      expect(initial).toMatchObject({
        ok: true,
        origin: 'http://localhost:3000/ota/',
        default_origin: 'http://localhost:3000/ota/',
        overridden: false,
        version_floor: '0.4.0',
        running_version: '0.4.0',
      });

      const set = await transport.sendRPC<any>('bramble.otaSetOrigin', { origin: 'https://custom.example/ota/' });
      expect(set).toMatchObject({ ok: true, origin: 'https://custom.example/ota/', overridden: true });

      const afterSet = await transport.sendRPC<any>('bramble.otaGetOrigin');
      expect(afterSet.origin).toBe('https://custom.example/ota/');
      expect(afterSet.overridden).toBe(true);

      const reset = await transport.sendRPC<any>('bramble.otaSetOrigin', { reset: true });
      expect(reset).toMatchObject({ ok: true, overridden: false });
      const afterReset = await transport.sendRPC<any>('bramble.otaGetOrigin');
      expect(afterReset.origin).toBe('http://localhost:3000/ota/');
      expect(afterReset.overridden).toBe(false);
    });

    it('otaUpdate on a failing path emits downloading ticks then a failed state', async () => {
      const transport = new MockTransport();
      await transport.connect();

      const events: any[] = [];
      transport.onNotification((method, params) => {
        if (method === 'bramble.onOtaEvent') events.push(params);
      });

      const start = await transport.sendRPC<any>('bramble.otaUpdate', { path: 'stable/vfail/heltec-v4/bramble.bin' });
      expect(start.ok).toBe(true);

      await vi.advanceTimersByTimeAsync(200);

      const states = events.map((e) => e.state);
      expect(states).toEqual(['downloading', 'downloading', 'failed']);
      expect(events[1].percent).toBe(40);
      expect(events[2].error).toBe('mock: simulated failure');

      const status = await transport.sendRPC<any>('bramble.otaStatus');
      expect(status.state).toBe('failed');
      expect(status.last_error).toBe('mock: simulated failure');
    });

    it('simulates a full OTA event stream ending in rebooting, flips running_version to 0.5.0', async () => {
      const transport = new MockTransport();
      await transport.connect();

      const events: any[] = [];
      transport.onNotification((method, params) => {
        if (method === 'bramble.onOtaEvent') events.push(params);
      });

      const start = await transport.sendRPC<any>('bramble.otaUpdate', { path: 'stable/v0.5.0/heltec-v4/bramble.bin' });
      expect(start.ok).toBe(true);

      await vi.advanceTimersByTimeAsync(200);

      const states = events.map((e) => e.state);
      expect(states[0]).toBe('downloading');
      expect(states).toContain('verifying');
      expect(states[states.length - 1]).toBe('rebooting');

      const status = await transport.sendRPC<any>('bramble.otaStatus');
      expect(status.state).toBe('rebooting');
      expect(status.running_version).toBe('0.5.0');

      const origin = await transport.sendRPC<any>('bramble.otaGetOrigin');
      expect(origin.running_version).toBe('0.5.0');
    });
  });
});
