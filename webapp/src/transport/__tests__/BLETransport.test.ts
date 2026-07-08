import { afterEach, describe, expect, it, vi } from 'vitest';
import { BLETransport } from '../BLETransport';

// Minimal fake Web Bluetooth stack: a TX characteristic that records writes
// and an RX characteristic that fires 'characteristicvaluechanged' the same
// way the real one does, so BLETransport's real connect()/processLines()
// logic runs unmodified against it.
function makeFakeBleStack() {
  const writes: Uint8Array[] = [];
  const rxListeners: Array<(e: Event) => void> = [];

  const txChar = {
    writeValueWithResponse: vi.fn(async (data: BufferSource) => {
      writes.push(new Uint8Array(data as ArrayBuffer));
    }),
  };

  const rxChar = {
    startNotifications: vi.fn(async () => rxChar),
    addEventListener: vi.fn((_type: string, cb: (e: Event) => void) => {
      rxListeners.push(cb);
    }),
    removeEventListener: vi.fn((_type: string, cb: (e: Event) => void) => {
      const i = rxListeners.indexOf(cb);
      if (i >= 0) rxListeners.splice(i, 1);
    }),
  };

  function emitLine(line: string) {
    const bytes = new TextEncoder().encode(line);
    const event = {
      target: { value: bytes.buffer },
    } as unknown as Event;
    for (const cb of rxListeners) cb(event);
  }

  const service = {
    getCharacteristic: vi.fn(async (uuid: string) => {
      if (uuid === '6e400002-b5a3-f393-e0a9-e50e24dcca9e') return txChar;
      if (uuid === '6e400003-b5a3-f393-e0a9-e50e24dcca9e') return rxChar;
      throw new Error(`unknown characteristic ${uuid}`);
    }),
  };

  const gattServer = {
    connect: vi.fn(async () => gattServer),
    getPrimaryService: vi.fn(async () => service),
    connected: true,
    disconnect: vi.fn(),
  };

  const deviceListeners: Record<string, Array<() => void>> = {};
  const device = {
    gatt: gattServer,
    addEventListener: vi.fn((type: string, cb: () => void) => {
      (deviceListeners[type] ??= []).push(cb);
    }),
  };
  function fireGattDisconnected() {
    for (const cb of deviceListeners['gattserverdisconnected'] ?? []) cb();
  }

  const bluetooth = {
    requestDevice: vi.fn(async () => device),
  };

  return { bluetooth, txChar, rxChar, gattServer, writes, emitLine, fireGattDisconnected, rxListeners };
}

function writesAsText(writes: Uint8Array[]): string {
  return writes.map(w => new TextDecoder().decode(w)).join('');
}

describe('BLETransport auth handshake', () => {
  afterEach(() => {
    vi.unstubAllGlobals();
  });

  it('writes the token line first and resolves connect() on an ok result', async () => {
    const { bluetooth, writes, emitLine } = makeFakeBleStack();
    vi.stubGlobal('navigator', { bluetooth });

    const transport = new BLETransport('secret-token');
    const connectPromise = transport.connect();

    // Let requestDevice/gatt.connect/getCharacteristic/startNotifications settle.
    await vi.waitFor(() => expect(writesAsText(writes)).toBe('secret-token\n'));
    expect(transport.connected).toBe(false); // not yet resolved: awaiting auth result

    emitLine('{"jsonrpc":"2.0","result":{"ok":true},"id":null}\n');

    await connectPromise;
    expect(transport.connected).toBe(true);
    expect(writesAsText(writes)).toBe('secret-token\n');
  });

  it('rejects connect() on an auth error result', async () => {
    const { bluetooth, writes, emitLine } = makeFakeBleStack();
    vi.stubGlobal('navigator', { bluetooth });

    const transport = new BLETransport('wrong-token');
    const connectPromise = transport.connect();

    await vi.waitFor(() => expect(writesAsText(writes)).toBe('wrong-token\n'));

    emitLine('{"jsonrpc":"2.0","error":{"code":-32000,"message":"unauthorized"},"id":null}\n');

    await expect(connectPromise).rejects.toThrow(/unauthorized|auth/i);
    expect(transport.connected).toBe(false);
  });

  it('times out and rejects connect() if no auth result arrives', async () => {
    vi.useFakeTimers();
    try {
      const { bluetooth } = makeFakeBleStack();
      vi.stubGlobal('navigator', { bluetooth });

      const transport = new BLETransport('slow-token');
      const connectPromise = transport.connect();
      connectPromise.catch(() => {});

      // Flush the microtask chain (requestDevice -> gatt.connect -> ... ->
      // writeChunked) so the auth-timeout timer is armed before advancing.
      await vi.advanceTimersByTimeAsync(0);
      await vi.advanceTimersByTimeAsync(6000);

      await expect(connectPromise).rejects.toThrow(/timed out/i);
    } finally {
      vi.useRealTimers();
    }
  });

  it('does not write a handshake when no token is provided', async () => {
    const { bluetooth, writes } = makeFakeBleStack();
    vi.stubGlobal('navigator', { bluetooth });

    const transport = new BLETransport();
    await transport.connect();

    expect(transport.connected).toBe(true);
    expect(writes.length).toBe(0);
  });

  it('routes a normal RPC response correctly after a successful handshake', async () => {
    const { bluetooth, writes, emitLine } = makeFakeBleStack();
    vi.stubGlobal('navigator', { bluetooth });

    const transport = new BLETransport('secret-token');
    const connectPromise = transport.connect();
    await vi.waitFor(() => expect(writesAsText(writes)).toBe('secret-token\n'));
    emitLine('{"jsonrpc":"2.0","result":{"ok":true},"id":null}\n');
    await connectPromise;

    const rpcPromise = transport.sendRPC<{ pong: boolean }>('bramble.ping', {}, 1000);
    // Wait for sendRPC to finish writing all chunks and register the pending
    // request before delivering the response line.
    await vi.waitFor(() => expect((transport as unknown as { pending: Map<number, unknown> }).pending.size).toBe(1));
    emitLine('{"jsonrpc":"2.0","id":1,"result":{"pong":true}}\n');

    await expect(rpcPromise).resolves.toEqual({ pong: true });
  });

  it('serializes concurrent sendRPC writes so chunks never interleave', async () => {
    const { bluetooth, txChar, writes, emitLine } = makeFakeBleStack();
    // Yield to the event loop inside each write so unserialized concurrent
    // callers WOULD interleave their chunks (reproduces the corrupted-line
    // failure that made sends time out while background polls ran).
    txChar.writeValueWithResponse.mockImplementation(async (data: BufferSource) => {
      await new Promise(r => setTimeout(r, 0));
      writes.push(new Uint8Array(data as ArrayBuffer));
    });
    vi.stubGlobal('navigator', { bluetooth });

    const transport = new BLETransport();
    await transport.connect();

    const a = transport.sendRPC('bramble.sendBroadcast', { text: 'a'.repeat(80) }, 2000).catch(() => {});
    const b = transport.sendRPC('bramble.getStatus', { filler: 'b'.repeat(80) }, 2000).catch(() => {});
    await vi.waitFor(() => {
      const lines = writesAsText(writes).split('\n').filter(l => l.length > 0);
      expect(lines).toHaveLength(2);
    });

    // Every reassembled line must be intact JSON: interleaved chunks would
    // corrupt both.
    const lines = writesAsText(writes).split('\n').filter(l => l.length > 0);
    for (const line of lines) {
      expect(() => JSON.parse(line)).not.toThrow();
    }
    // Settle the pending RPCs so no timers leak into other tests.
    emitLine('{"jsonrpc":"2.0","id":1,"result":{}}\n');
    emitLine('{"jsonrpc":"2.0","id":2,"result":{}}\n');
    await Promise.all([a, b]);
  });
});

describe('BLETransport auto-reconnect', () => {
  afterEach(() => {
    vi.unstubAllGlobals();
    vi.useRealTimers();
  });

  async function connectWithAuth(stack: ReturnType<typeof makeFakeBleStack>) {
    const transport = new BLETransport('secret-token');
    const p = transport.connect();
    await vi.waitFor(() => expect(writesAsText(stack.writes)).toContain('secret-token\n'));
    stack.emitLine('{"jsonrpc":"2.0","result":{"ok":true},"id":null}\n');
    await p;
    return transport;
  }

  it('re-establishes the link (including re-auth) after an unexpected drop', async () => {
    const stack = makeFakeBleStack();
    vi.stubGlobal('navigator', { bluetooth: stack.bluetooth });
    const transport = await connectWithAuth(stack);

    const onDisconnect = vi.fn();
    const onReconnect = vi.fn();
    transport.enableAutoReconnect({ onDisconnect, onReconnect });

    expect(stack.gattServer.connect).toHaveBeenCalledTimes(1);
    stack.fireGattDisconnected();
    expect(transport.connected).toBe(false);
    expect(onDisconnect).toHaveBeenCalledTimes(1);

    // First retry fires after the initial backoff and redoes the handshake.
    await vi.waitFor(() => expect(stack.gattServer.connect).toHaveBeenCalledTimes(2), { timeout: 5000 });
    await vi.waitFor(() => {
      const tokenWrites = writesAsText(stack.writes).split('secret-token\n').length - 1;
      expect(tokenWrites).toBe(2);
    });
    stack.emitLine('{"jsonrpc":"2.0","result":{"ok":true},"id":null}\n');

    await vi.waitFor(() => expect(onReconnect).toHaveBeenCalledTimes(1));
    expect(transport.connected).toBe(true);
    // The reconnect must not have stacked a duplicate RX listener (the
    // polyfill returns the same characteristic object across reconnects).
    expect(stack.rxListeners.length).toBe(1);
  });

  it('keeps retrying with backoff while the node stays out of range', async () => {
    const stack = makeFakeBleStack();
    vi.stubGlobal('navigator', { bluetooth: stack.bluetooth });
    const transport = await connectWithAuth(stack);
    transport.enableAutoReconnect({});

    // Every reconnect attempt fails: still out of range.
    stack.gattServer.connect.mockRejectedValue(new Error('out of range'));
    stack.fireGattDisconnected();

    await vi.waitFor(() => expect(stack.gattServer.connect.mock.calls.length).toBeGreaterThanOrEqual(2), { timeout: 10000 });
    expect(transport.connected).toBe(false);

    // Node back in range: next attempt succeeds and re-auths.
    stack.gattServer.connect.mockImplementation(async () => stack.gattServer);
    await vi.waitFor(() => {
      const tokenWrites = writesAsText(stack.writes).split('secret-token\n').length - 1;
      expect(tokenWrites).toBe(2);
    }, { timeout: 10000 });
    stack.emitLine('{"jsonrpc":"2.0","result":{"ok":true},"id":null}\n');
    await vi.waitFor(() => expect(transport.connected).toBe(true));
  }, 20000);

  it('recovers even when a write was stuck in flight at disconnect (wedge regression)', async () => {
    const stack = makeFakeBleStack();
    vi.stubGlobal('navigator', { bluetooth: stack.bluetooth });
    const transport = await connectWithAuth(stack);
    const onReconnect = vi.fn();
    transport.enableAutoReconnect({ onReconnect });

    // Simulate the Android bridge losing the link mid-write: the pending
    // write promise NEVER settles. Without the queue reset in
    // establishLink, the reconnect auth write queues behind it forever.
    stack.txChar.writeValueWithResponse.mockImplementationOnce(
      () => new Promise(() => {})
    );
    transport.sendRPC('bramble.getStatus', {}, 1000).catch(() => {});
    await new Promise(r => setTimeout(r, 10)); // let the doomed write start
    stack.fireGattDisconnected();

    // Reconnect attempt must still write the token on the fresh session.
    await vi.waitFor(() => {
      const tokenWrites = writesAsText(stack.writes).split('secret-token\n').length - 1;
      expect(tokenWrites).toBe(2);
    }, { timeout: 8000 });
    stack.emitLine('{"jsonrpc":"2.0","result":{"ok":true},"id":null}\n');
    await vi.waitFor(() => expect(onReconnect).toHaveBeenCalledTimes(1));
    expect(transport.connected).toBe(true);
  }, 15000);

  it('tears down a half-open GATT connection when a reconnect attempt fails', async () => {
    const stack = makeFakeBleStack();
    vi.stubGlobal('navigator', { bluetooth: stack.bluetooth });
    const transport = await connectWithAuth(stack);
    transport.enableAutoReconnect({});

    // gatt.connect succeeds but auth times out: half-open session. The
    // retry loop must disconnect it so the node resumes advertising.
    stack.fireGattDisconnected();
    // Swallow the token write on the retry so the handshake times out, and
    // report the gatt as connected so teardown has something to close.
    stack.txChar.writeValueWithResponse.mockImplementation(() => new Promise(() => {}));
    await vi.waitFor(
      () => expect(stack.gattServer.disconnect).toHaveBeenCalled(),
      { timeout: 12000 }
    );
    expect(transport.connected).toBe(false);
  }, 20000);

  it('does not reconnect after an intentional disconnect()', async () => {
    const stack = makeFakeBleStack();
    vi.stubGlobal('navigator', { bluetooth: stack.bluetooth });
    const transport = await connectWithAuth(stack);
    const onDisconnect = vi.fn();
    transport.enableAutoReconnect({ onDisconnect });

    await transport.disconnect();
    stack.fireGattDisconnected();

    await new Promise(r => setTimeout(r, 50));
    expect(onDisconnect).not.toHaveBeenCalled();
    expect(stack.gattServer.connect).toHaveBeenCalledTimes(1);
  });
});
