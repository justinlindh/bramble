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
    stopNotifications: vi.fn(async () => rxChar),
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
    connect: vi.fn(async () => { gattServer.connected = true; return gattServer; }),
    getPrimaryService: vi.fn(async () => service),
    // Models real Web Bluetooth: disconnected until connect() resolves.
    connected: false,
    disconnect: vi.fn(() => { gattServer.connected = false; }),
  };

  const deviceListeners: Record<string, Array<() => void>> = {};
  const device = {
    gatt: gattServer,
    addEventListener: vi.fn((type: string, cb: () => void) => {
      (deviceListeners[type] ??= []).push(cb);
    }),
  };
  function fireGattDisconnected() {
    gattServer.connected = false;
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
    const { bluetooth, writes, emitLine, gattServer } = makeFakeBleStack();
    vi.stubGlobal('navigator', { bluetooth });

    const transport = new BLETransport('wrong-token');
    const connectPromise = transport.connect();

    await vi.waitFor(() => expect(writesAsText(writes)).toBe('wrong-token\n'));

    emitLine('{"jsonrpc":"2.0","error":{"code":-32000,"message":"unauthorized"},"id":null}\n');

    await expect(connectPromise).rejects.toThrow(/unauthorized|auth/i);
    expect(transport.connected).toBe(false);
    // The rejection must not leave the GATT link standing: the store never
    // gets a client to clean up when connect() itself rejects, and a
    // peripheral with an open link stops advertising, so a leaked link makes
    // the node invisible to every later chooser.
    expect(gattServer.disconnect).toHaveBeenCalled();
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
      // connect() retries the link ONCE after a timeout (stale-session
      // recovery), so advance through both attempts plus the retry delay.
      await vi.advanceTimersByTimeAsync(0);
      await vi.advanceTimersByTimeAsync(6000);
      await vi.advanceTimersByTimeAsync(2000);
      await vi.advanceTimersByTimeAsync(6000);

      await expect(connectPromise).rejects.toThrow(/timed out/i);
    } finally {
      vi.useRealTimers();
    }
  });

  it('retries the link when the first write fails the platform GATT security check', async () => {
    // The firmware requires an encrypted link (issue #73), so on an unpaired
    // device the very first write can fail while the OS runs its pairing
    // prompt. The grace machinery retries the token write on the SAME link
    // attempt (a second link attempt would raise a second OS prompt) until
    // the write goes through. That message contains "authorized", which a
    // looser substring match once mistook for a token rejection and threw
    // instead of retrying.
    const { bluetooth, txChar, writes, emitLine } = makeFakeBleStack();
    vi.stubGlobal('navigator', { bluetooth });

    let firstAttempt = true;
    txChar.writeValueWithResponse.mockImplementation(async (data: BufferSource) => {
      if (firstAttempt) {
        firstAttempt = false;
        throw new Error('GATT operation not authorized');
      }
      writes.push(new Uint8Array(data as ArrayBuffer));
    });

    const transport = new BLETransport('secret-token');
    const connectPromise = transport.connect();

    // The retry attempt writes the token again; answer it.
    await vi.waitFor(() => expect(writesAsText(writes)).toBe('secret-token\n'), { timeout: 5000 });
    emitLine('{"jsonrpc":"2.0","result":{"ok":true},"id":null}\n');

    await connectPromise;
    expect(transport.connected).toBe(true);
  });

  it('writes an encryption probe (never a bare token line) when no token is provided', async () => {
    // The firmware's TX characteristic requires an encrypted link, so the
    // first write triggers the OS pairing prompt. Without a token the old
    // code wrote nothing during connect(), which deferred pairing to the
    // first real RPC (getVersion) where its 4-5s timeout aborted the prompt.
    // The probe is JSON, so an auth-required node routes it to the
    // unauthenticated allowlist dispatcher instead of counting it as a
    // failed token attempt (ble_rpc_task in ble_server.c).
    const { bluetooth, writes } = makeFakeBleStack();
    vi.stubGlobal('navigator', { bluetooth });

    const transport = new BLETransport();
    await transport.connect();

    expect(transport.connected).toBe(true);
    const text = writesAsText(writes);
    expect(text.startsWith('{')).toBe(true);
    expect(text).toContain('"id":0'); // real RPC ids start at 1: the reply routes nowhere
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
    await vi.waitFor(() => expect((transport as unknown as { rpc: { pending: Map<number, unknown> } }).rpc.pending.size).toBe(1));
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
      const text = writesAsText(writes);
      // Wait for COMPLETED lines: a trailing partial line would parse-fail
      // below. Three lines: the no-token connect()'s encryption probe plus
      // the two RPCs under test.
      expect(text.endsWith('\n')).toBe(true);
      expect(text.split('\n').filter(l => l.length > 0)).toHaveLength(3);
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

describe('BLETransport first-time pairing', () => {
  afterEach(() => {
    vi.unstubAllGlobals();
    vi.useRealTimers();
  });

  it('keeps retrying the token write while the OS pairing prompt is up, on one link attempt', async () => {
    // Fail-fast stacks (BlueZ) reject the write with a security error while
    // the user is still typing the passkey. The old code let the 5s
    // handshake timer abort the whole connect mid-pairing and then blind-
    // retried the link, producing a second prompt and feeding the node's
    // anti-MITM advertising backoff.
    vi.useFakeTimers();
    const { bluetooth, txChar, writes, emitLine, gattServer } = makeFakeBleStack();
    vi.stubGlobal('navigator', { bluetooth });

    let paired = false;
    txChar.writeValueWithResponse.mockImplementation(async (data: BufferSource) => {
      if (!paired) throw new Error('GATT operation not authorized');
      writes.push(new Uint8Array(data as ArrayBuffer));
    });

    const pairingStates: boolean[] = [];
    const transport = new BLETransport('secret-token');
    transport.onPairingStateChange(s => pairingStates.push(s));
    const connectPromise = transport.connect();
    connectPromise.catch(() => {});

    await vi.advanceTimersByTimeAsync(0);
    // 12s of typing: far beyond the old 5s handshake / 6s write timeouts.
    await vi.advanceTimersByTimeAsync(12000);
    expect(transport.connected).toBe(false);
    paired = true;
    await vi.advanceTimersByTimeAsync(1000); // next grace retry lands the token

    expect(writesAsText(writes)).toBe('secret-token\n');
    emitLine('{"jsonrpc":"2.0","result":{"ok":true},"id":null}\n');
    await connectPromise;

    expect(transport.connected).toBe(true);
    expect(gattServer.connect).toHaveBeenCalledTimes(1); // one link attempt = one OS prompt
    expect(gattServer.disconnect).not.toHaveBeenCalled(); // never torn down mid-pairing
    expect(pairingStates).toEqual([true, false]);
  });

  it('survives a token write that blocks past the old 6s timeout (Chrome pairing dialog)', async () => {
    // Chrome resolves an insufficient-authentication write only after its own
    // pairing dialog completes: the write promise simply blocks while the
    // user types. The old writeWithTimeout fired at 6s and called
    // gatt.disconnect(), killing SMP mid-entry.
    vi.useFakeTimers();
    const { bluetooth, txChar, writes, emitLine, gattServer } = makeFakeBleStack();
    vi.stubGlobal('navigator', { bluetooth });

    let resolveWrite: (() => void) | null = null;
    txChar.writeValueWithResponse.mockImplementationOnce((data: BufferSource) =>
      new Promise<void>(r => {
        resolveWrite = () => { writes.push(new Uint8Array(data as ArrayBuffer)); r(); };
      })
    );

    const transport = new BLETransport('secret-token');
    const connectPromise = transport.connect();
    connectPromise.catch(() => {});

    await vi.advanceTimersByTimeAsync(0);
    await vi.advanceTimersByTimeAsync(15000); // user still typing at 15s
    expect(gattServer.disconnect).not.toHaveBeenCalled();
    expect(transport.connected).toBe(false);

    resolveWrite!();
    await vi.advanceTimersByTimeAsync(0);
    expect(writesAsText(writes)).toBe('secret-token\n');
    emitLine('{"jsonrpc":"2.0","result":{"ok":true},"id":null}\n');
    await connectPromise;

    expect(transport.connected).toBe(true);
    expect(gattServer.connect).toHaveBeenCalledTimes(1);
  });

  it('gives up after the pairing grace window with a pairing error, not a raw GATT string', async () => {
    (BLETransport as unknown as { pairingGraceMs: number }).pairingGraceMs = 2000;
    vi.useFakeTimers();
    try {
      const { bluetooth, txChar, gattServer } = makeFakeBleStack();
      vi.stubGlobal('navigator', { bluetooth });
      txChar.writeValueWithResponse.mockRejectedValue(new Error('GATT operation not authorized'));

      const transport = new BLETransport('secret-token');
      const connectPromise = transport.connect();
      const rejection = expect(connectPromise).rejects.toThrow(/pairing did not complete/i);

      await vi.advanceTimersByTimeAsync(0);
      await vi.advanceTimersByTimeAsync(2500); // grace expires on attempt 1
      await vi.advanceTimersByTimeAsync(1600); // stale-session retry delay
      await vi.advanceTimersByTimeAsync(500);  // attempt 2 fails fast (no second grace)
      await rejection;

      // The prompt is long dead by expiry, so one fresh-link retry is safe;
      // more would re-prompt.
      expect(gattServer.connect).toHaveBeenCalledTimes(2);
    } finally {
      (BLETransport as unknown as { pairingGraceMs: number }).pairingGraceMs = 60000;
    }
  });

  it('aborts immediately when the user cancels the pairing dialog, with no link retry', async () => {
    const { bluetooth, txChar, writes, gattServer } = makeFakeBleStack();
    vi.stubGlobal('navigator', { bluetooth });
    txChar.writeValueWithResponse.mockRejectedValue(new Error('Authentication canceled.'));

    const transport = new BLETransport('secret-token');
    await expect(transport.connect()).rejects.toThrow(/cancel/i);

    expect(gattServer.connect).toHaveBeenCalledTimes(1); // a retry would re-prompt
    expect(writes.length).toBe(0);
    // The abort must still tear the link down: at cancel time gatt.connect()
    // and discovery already succeeded (neither needs encryption), and a
    // standing link keeps the node from advertising, which is exactly the
    // connected-but-invisible state the user escapes by power-cycling.
    expect(gattServer.disconnect).toHaveBeenCalled();
  });

  it('ignores a dead session\'s stale write timer after auto-reconnect built a new session', async () => {
    // A hung write's timeout used to call gatt.disconnect() unconditionally.
    // If the link died and auto-reconnect already established a NEW healthy
    // session by the time that stale timer fired, the timer killed the new
    // session. The session-generation guard must make the stale timer a
    // no-op while still letting same-session hung writes drop the link.
    (BLETransport as unknown as { writeChunkTimeoutMs: number }).writeChunkTimeoutMs = 3000;
    try {
      const stack = makeFakeBleStack();
      vi.stubGlobal('navigator', { bluetooth: stack.bluetooth });
      const transport = new BLETransport('secret-token');
      const p = transport.connect();
      await vi.waitFor(() => expect(writesAsText(stack.writes)).toContain('secret-token\n'));
      stack.emitLine('{"jsonrpc":"2.0","result":{"ok":true},"id":null}\n');
      await p;
      const onReconnect = vi.fn();
      transport.enableAutoReconnect({ onReconnect });

      // Session 1: a write hangs (its 3s timer now pending), then the link
      // drops and auto-reconnect rebuilds within ~2s: session 2 is healthy
      // and _connected when the stale 3s timer fires.
      stack.txChar.writeValueWithResponse.mockImplementationOnce(() => new Promise(() => {}));
      transport.sendRPC('bramble.getStatus', {}, 10000).catch(() => {});
      await new Promise(r => setTimeout(r, 10)); // let the doomed write start
      stack.fireGattDisconnected();
      await vi.waitFor(() => {
        const tokenWrites = writesAsText(stack.writes).split('secret-token\n').length - 1;
        expect(tokenWrites).toBe(2);
      }, { timeout: 8000 });
      stack.emitLine('{"jsonrpc":"2.0","result":{"ok":true},"id":null}\n');
      await vi.waitFor(() => expect(onReconnect).toHaveBeenCalledTimes(1));
      const disconnectsAfterReconnect = stack.gattServer.disconnect.mock.calls.length;

      // Ride past the stale timer's 3s mark: it must not touch the new session.
      await new Promise(r => setTimeout(r, 3300));
      expect(stack.gattServer.disconnect.mock.calls.length).toBe(disconnectsAfterReconnect);
      expect(transport.connected).toBe(true);
    } finally {
      (BLETransport as unknown as { writeChunkTimeoutMs: number }).writeChunkTimeoutMs = 6000;
    }
  }, 20000);
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

  it('survives a native connect that never settles (loop-death regression)', async () => {
    (BLETransport as any).establishLinkTimeoutMs = 300;
    try {
      const stack = makeFakeBleStack();
      vi.stubGlobal('navigator', { bluetooth: stack.bluetooth });
      const transport = await connectWithAuth(stack);
      const onReconnect = vi.fn();
      transport.enableAutoReconnect({ onReconnect });

      // Android's BLE stack can never deliver the connect callback after a
      // peer dies mid-connection: model it as a gatt.connect that hangs.
      let hangs = 0;
      stack.gattServer.connect.mockImplementation(() => {
        if (hangs++ < 2) return new Promise(() => {});
        return Promise.resolve(stack.gattServer);
      });
      stack.fireGattDisconnected();

      // Two hanging attempts must both time out and the third succeed.
      await vi.waitFor(() => {
        const tokenWrites = writesAsText(stack.writes).split('secret-token\n').length - 1;
        expect(tokenWrites).toBe(2);
      }, { timeout: 15000 });
      stack.emitLine('{"jsonrpc":"2.0","result":{"ok":true},"id":null}\n');
      await vi.waitFor(() => expect(onReconnect).toHaveBeenCalledTimes(1));
      expect(transport.connected).toBe(true);
    } finally {
      (BLETransport as any).establishLinkTimeoutMs = 20000;
    }
  }, 20000);

  it('retries immediately when the app becomes visible during backoff', async () => {
    const stack = makeFakeBleStack();
    vi.stubGlobal('navigator', { bluetooth: stack.bluetooth });
    const transport = await connectWithAuth(stack);
    transport.enableAutoReconnect({});

    // Fail attempts so the backoff grows, then fire visibilitychange: the
    // next attempt must come promptly, not after the stretched delay.
    stack.gattServer.connect.mockRejectedValue(new Error('out of range'));
    stack.fireGattDisconnected();
    await vi.waitFor(() => expect(stack.gattServer.connect.mock.calls.length).toBeGreaterThanOrEqual(2), { timeout: 10000 });

    stack.gattServer.connect.mockImplementation(async () => stack.gattServer);
    const callsBefore = stack.gattServer.connect.mock.calls.length;
    Object.defineProperty(document, 'visibilityState', { value: 'visible', configurable: true });
    document.dispatchEvent(new Event('visibilitychange'));
    await vi.waitFor(() => expect(stack.gattServer.connect.mock.calls.length).toBeGreaterThan(callsBefore), { timeout: 4000 });
  }, 20000);

  it('times out a hung GATT write and drops the link (silent-wedge regression)', async () => {
    (BLETransport as any).writeChunkTimeoutMs = 250;
    try {
      const stack = makeFakeBleStack();
      vi.stubGlobal('navigator', { bluetooth: stack.bluetooth });
      const transport = await connectWithAuth(stack);
      const onDisconnect = vi.fn();
      transport.enableAutoReconnect({ onDisconnect });

      // Desktop BlueZ failure mode: a write that never completes while the
      // GATT link still claims connected. The RPC must fail (not hang) and
      // the transport must drop the link so auto-reconnect can heal it.
      stack.txChar.writeValueWithResponse.mockImplementation(() => new Promise(() => {}));
      await expect(transport.sendRPC('bramble.getStatus', {}, 5000)).rejects.toThrow(/write timed out/i);
      expect(stack.gattServer.disconnect).toHaveBeenCalled();
    } finally {
      (BLETransport as any).writeChunkTimeoutMs = 6000;
    }
  }, 10000);

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
