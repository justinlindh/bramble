import { describe, it, expect, vi, afterEach } from 'vitest';
import { SerialTransport } from '../../src/transport/SerialTransport';

const enc = new TextEncoder();

// ── Helpers ──────────────────────────────────────────────────────────────────

function makeReaderWithHook(chunks: Uint8Array[] = []) {
  let idx = 0;
  // Extra promise that tests can resolve to inject chunks mid-test
  let deliverChunk: ((chunk: Uint8Array) => void) | null = null;
  let pendingRead: (() => void) | null = null;

  const reader = {
    read: vi.fn(async (): Promise<{ value: Uint8Array; done: false } | { value: undefined; done: true }> => {
      if (idx < chunks.length) {
        return { value: chunks[idx++], done: false };
      }
      // Wait for a chunk to be injected or for cancel
      return new Promise(resolve => {
        pendingRead = () => {
          // This branch is not used in a cancellable way; just hang
          resolve({ value: new Uint8Array(), done: false });
        };
      });
    }),
    cancel: vi.fn(async () => {
      // Resolve any pending read with a done signal by ignoring it;
      // the read loop catches the _connected flag
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
    removeSerialMock(); // ensure serial is absent
    const t = new SerialTransport();
    await expect(t.connect()).rejects.toThrow('Web Serial API not supported');
  });

  it('rejects pending RPCs on disconnect', async () => {
    const { port } = makePort();
    installSerialMock(port);

    const t = new SerialTransport();
    await t.connect();

    // Start an RPC with a long timeout — it will never get a response
    const rpcPromise = t.sendRPC('bramble.getStatus', {}, 30000);

    // Disconnect; should reject all pending RPCs
    await t.disconnect();

    await expect(rpcPromise).rejects.toThrow('Disconnected');
  });

  it('handles RPC timeout', async () => {
    const { port } = makePort();
    installSerialMock(port);

    const t = new SerialTransport();
    await t.connect();

    // Use a very short timeout so the test doesn't take long
    const rpcPromise = t.sendRPC('bramble.getStatus', {}, 50);

    await expect(rpcPromise).rejects.toThrow('RPC timeout: bramble.getStatus');

    await t.disconnect();
  }, 5000);

  it('routes notifications correctly', async () => {
    const notif = { jsonrpc: '2.0', method: 'bramble.onMessage', params: { text: 'hello' } };
    const notifLine = enc.encode(JSON.stringify(notif) + '\n');

    const { port, reader } = makePort();
    installSerialMock(port);

    const t = new SerialTransport();
    const received: { method: string; params: unknown }[] = [];
    t.onNotification((method, params) => received.push({ method, params }));

    await t.connect();

    // Inject the notification chunk
    reader.inject(notifLine);

    // Wait a bit for the async read loop to process
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

    // Allow the sendRPC write to complete before injecting response
    await Promise.resolve();
    reader.inject(responseLine);

    const result = await rpcPromise;
    expect(result).toEqual({ ok: true });

    await t.disconnect();
  }, 5000);

  it('handles multiple response lines in a single chunk', async () => {
    const resp1 = { jsonrpc: '2.0', id: 1, result: { a: 1 } };
    const resp2 = { jsonrpc: '2.0', id: 2, result: { b: 2 } };
    const combined = enc.encode(JSON.stringify(resp1) + '\n' + JSON.stringify(resp2) + '\n');

    const { port, reader } = makePort();
    installSerialMock(port);

    const t = new SerialTransport();
    await t.connect();

    const p1 = t.sendRPC<{ a: number }>('bramble.getA', {}, 2000);
    const p2 = t.sendRPC<{ b: number }>('bramble.getB', {}, 2000);

    await Promise.resolve();
    reader.inject(combined);

    const [r1, r2] = await Promise.all([p1, p2]);
    expect(r1).toEqual({ a: 1 });
    expect(r2).toEqual({ b: 2 });

    await t.disconnect();
  }, 5000);

  it('throws if sendRPC is called while disconnected', async () => {
    const { port } = makePort();
    installSerialMock(port);

    const t = new SerialTransport();
    // Not connected yet
    await expect(t.sendRPC('bramble.getStatus')).rejects.toThrow('Not connected');
  });
});
