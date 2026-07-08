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

  it('disconnect() tears down the fake socket and future RPCs fail', async () => {
    const transport = new MockTransport();
    await transport.connect();
    await transport.disconnect();

    expect(transport.connected).toBe(false);
    await expect(transport.sendRPC('bramble.getConfig')).rejects.toThrow(/Not connected/);
  });
});
