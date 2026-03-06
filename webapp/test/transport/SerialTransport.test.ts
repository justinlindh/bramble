import { describe, it, expect, vi, afterEach } from 'vitest';
import { SerialTransport } from '../../src/transport/SerialTransport';

const enc = new TextEncoder();

// ── Helpers ──────────────────────────────────────────────────────────────────

function makeReaderWithHook(chunks: Uint8Array[] = []) {
  let idx = 0;
  let pendingRead: (() => void) | null = null;

  const reader = {
    read: vi.fn(async (): Promise<{ value: Uint8Array; done: false } | { value: undefined; done: true }> => {
      if (idx < chunks.length) {
        return { value: chunks[idx++], done: false };
      }
      return new Promise(resolve => {
        pendingRead = () => {
          resolve({ value: new Uint8Array(), done: false });
        };
      });
    }),
    cancel: vi.fn(async () => {
      if (pendingRead) pendingRead();
    }),
    releaseLock: vi.fn(),
    inject: (chunk: Uint8Array) => {
      chunks.push(chunk);
      if (pendingRead) {
        const fn = pendingRead;
        pendingRead = null;
        fn();
      }
    },
  };
  return reader;
}

function makeWritableStream() {
  const written: Uint8Array[] = [];
  return {
    written,
    getWriter: () => ({
      write: vi.fn(async (chunk: Uint8Array) => { written.push(chunk); }),
      releaseLock: vi.fn(),
    }),
  };
}

function makePort(readChunks: Uint8Array[] = []) {
  const writable = makeWritableStream();
  const reader = makeReaderWithHook(readChunks);
  const readable = { getReader: () => reader };
  return { port: { open: vi.fn(async () => {}), close: vi.fn(async () => {}), writable, readable }, reader };
}

function installSerialMock(port: ReturnType<typeof makePort>['port']) {
  Object.defineProperty(navigator, 'serial', {
    value: { requestPort: vi.fn(async () => port) },
    configurable: true,
    writable: true,
  });
}

function removeSerialMock() {
  Object.defineProperty(navigator, 'serial', {
    value: undefined,
    configurable: true,
    writable: true,
  });
}

afterEach(() => {
  vi.restoreAllMocks();
  removeSerialMock();
});

// ── Tests ─────────────────────────────────────────────────────────────────────

describe('SerialTransport', () => {
  it('throws if Web Serial not supported', async () => {
    removeSerialMock();
    const t = new SerialTransport();
    await expect(t.connect()).rejects.toThrow('Web Serial API not supported');
  });

  it('rejects pending RPCs on disconnect', async () => {
    const { port } = makePort();
    installSerialMock(port);

    const t = new SerialTransport();
    await t.connect();

    const rpcPromise = t.sendRPC('bramble.getStatus', {}, 30000);
    await t.disconnect();

    await expect(rpcPromise).rejects.toThrow();
  });

  it('rejects RPC on timeout', async () => {
    const { port } = makePort();
    installSerialMock(port);

    const t = new SerialTransport();
    await t.connect(); // drain uses real timers

    vi.useFakeTimers(); // switch to fake timers for RPC timeout testing
    try {
      const rpcPromise = t.sendRPC('bramble.getStatus', {}, 500);
      const assertion = expect(rpcPromise).rejects.toThrow('RPC timeout: bramble.getStatus');

      await vi.advanceTimersByTimeAsync(600);
      await assertion;

      await t.disconnect();
    } finally {
      vi.useRealTimers();
    }
  }, 10000);

  it('routes notifications correctly', async () => {
    const notif = { jsonrpc: '2.0', method: 'bramble.onMessage', params: { text: 'hello' } };
    const notifLine = enc.encode(JSON.stringify(notif) + '\n');

    const { port, reader } = makePort();
    installSerialMock(port);

    const t = new SerialTransport();
    const received: { method: string; params: unknown }[] = [];
    t.onNotification((method, params) => received.push({ method, params }));

    await t.connect();

    reader.inject(notifLine);
    await new Promise(r => setTimeout(r, 50));

    expect(received).toHaveLength(1);
    expect(received[0].method).toBe('bramble.onMessage');
    expect((received[0].params as { text: string }).text).toBe('hello');

    await t.disconnect();
  }, 5000);

  it('parses a successful RPC response', async () => {
    const response = { jsonrpc: '2.0', id: 1, result: { ok: true } };
    const responseLine = enc.encode(JSON.stringify(response) + '\n');

    const { port, reader } = makePort();
    installSerialMock(port);

    const t = new SerialTransport();
    await t.connect();

    const rpcPromise = t.sendRPC<{ ok: boolean }>('bramble.getStatus', {}, 2000);

    await Promise.resolve();
    reader.inject(responseLine);

    const result = await rpcPromise;
    expect(result).toEqual({ ok: true });

    await t.disconnect();
  }, 5000);

  it('handles sequential RPCs over serialized transport', async () => {
    const { port, reader } = makePort();
    installSerialMock(port);

    const t = new SerialTransport();
    await t.connect();

    // With serialization, RPC #2 waits for #1 to complete before writing.
    // Inject responses as they would arrive from firmware: one at a time.
    const p1 = t.sendRPC<{ a: number }>('bramble.getA', {}, 2000);
    const p2 = t.sendRPC<{ b: number }>('bramble.getB', {}, 2000);

    // Response for #1
    await new Promise(r => setTimeout(r, 10));
    reader.inject(enc.encode(JSON.stringify({ jsonrpc: '2.0', id: 1, result: { a: 1 } }) + '\n'));
    const r1 = await p1;
    expect(r1).toEqual({ a: 1 });

    // #1 resolved → #2's write fires → inject response for #2
    await new Promise(r => setTimeout(r, 10));
    reader.inject(enc.encode(JSON.stringify({ jsonrpc: '2.0', id: 2, result: { b: 2 } }) + '\n'));
    const r2 = await p2;
    expect(r2).toEqual({ b: 2 });

    await t.disconnect();
  }, 10000);

  it('reassembles fragmented JSON across multiple chunks', async () => {
    const response = JSON.stringify({ jsonrpc: '2.0', id: 1, result: { status: 'ok', uptime: 12345 } });
    // Split into tiny chunks like CP2102 would deliver
    const chunks = [];
    for (let i = 0; i < response.length; i += 4) {
      chunks.push(enc.encode(response.slice(i, i + 4)));
    }
    // Final newline in its own chunk
    chunks.push(enc.encode('\n'));

    const { port, reader } = makePort();
    installSerialMock(port);

    const t = new SerialTransport();
    await t.connect();

    const rpcPromise = t.sendRPC<{ status: string; uptime: number }>('bramble.getStatus', {}, 2000);

    // Deliver fragments with tiny delays (simulating USB serial)
    for (const chunk of chunks) {
      await Promise.resolve();
      reader.inject(chunk);
    }

    const result = await rpcPromise;
    expect(result).toEqual({ status: 'ok', uptime: 12345 });

    await t.disconnect();
  }, 5000);

  it('handles JSON mixed with firmware log noise', async () => {
    // Firmware spits log lines interleaved with JSON-RPC responses
    const noise = 'I (1234) wifi_mgr: scan complete\r\nbramble> ';
    const response = JSON.stringify({ jsonrpc: '2.0', id: 1, result: { ok: true } });
    const chunk = enc.encode(noise + response + '\r\n');

    const { port, reader } = makePort();
    installSerialMock(port);

    const t = new SerialTransport();
    await t.connect();

    const rpcPromise = t.sendRPC<{ ok: boolean }>('bramble.getStatus', {}, 2000);

    await Promise.resolve();
    reader.inject(chunk);

    const result = await rpcPromise;
    expect(result).toEqual({ ok: true });

    await t.disconnect();
  }, 5000);

  it('ignores echoed request frames (method+id but no result/error)', async () => {
    // Serial can echo the request back before the firmware responds
    const echoedRequest = JSON.stringify({ jsonrpc: '2.0', id: 1, method: 'bramble.getStatus', params: {} });
    const realResponse = JSON.stringify({ jsonrpc: '2.0', id: 1, result: { ok: true } });
    const chunk = enc.encode(echoedRequest + '\n' + realResponse + '\n');

    const { port, reader } = makePort();
    installSerialMock(port);

    const t = new SerialTransport();
    await t.connect();

    const rpcPromise = t.sendRPC<{ ok: boolean }>('bramble.getStatus', {}, 2000);

    await Promise.resolve();
    reader.inject(chunk);

    const result = await rpcPromise;
    expect(result).toEqual({ ok: true });

    await t.disconnect();
  }, 5000);

  it('throws if sendRPC is called while disconnected', async () => {
    const { port } = makePort();
    installSerialMock(port);

    const t = new SerialTransport();
    await expect(t.sendRPC('bramble.getStatus')).rejects.toThrow('Not connected');
  });
});
