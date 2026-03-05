import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { SerialTransport } from '../SerialTransport';

describe('SerialTransport serial JSON-RPC parsing (regressions)', () => {
  beforeEach(() => {
    vi.useFakeTimers();
  });

  afterEach(() => {
    vi.useRealTimers();
  });

  function setupConnectedTransport() {
    const transport = new SerialTransport();
    const writer = { write: vi.fn().mockResolvedValue(undefined) };

    (transport as any)._connected = true;
    (transport as any).writer = writer;

    return { transport, writer };
  }

  function feedChunk(transport: SerialTransport, chunk: string) {
    (transport as any).readBuf += chunk;
    (transport as any).processLines();
  }

  it('should parse JSON-RPC response even when log prefix appears before JSON on the same line', async () => {
    const { transport } = setupConnectedTransport();

    const rpcPromise = transport.sendRPC<{ ok: boolean }>('ping', {}, 50);

    feedChunk(
      transport,
      'I (1204) serial: tx complete {"jsonrpc":"2.0","id":1,"result":{"ok":true}}\n',
    );

    void rpcPromise.catch(() => {});
    const assertion = expect(rpcPromise).resolves.toEqual({ ok: true });
    await vi.advanceTimersByTimeAsync(60);
    await assertion;
  });

  it('should reconstruct and parse JSON-RPC response split across chunks with an internal newline', async () => {
    const { transport } = setupConnectedTransport();

    const rpcPromise = transport.sendRPC<{ ok: boolean }>('ping', {}, 50);

    feedChunk(transport, '{"jsonrpc":"2.0","id":1,\n');
    feedChunk(transport, '"result":{"ok":true}}\n');

    void rpcPromise.catch(() => {});
    const assertion = expect(rpcPromise).resolves.toEqual({ ok: true });
    await vi.advanceTimersByTimeAsync(60);
    await assertion;
  });

  it('should route notifications without id even when surrounded by serial log noise', () => {
    const { transport } = setupConnectedTransport();
    const onNotify = vi.fn();
    transport.onNotification(onNotify);

    feedChunk(
      transport,
      'D (99) dbg: start {"jsonrpc":"2.0","method":"mesh.event","params":{"node":7}} trailing bytes\n',
    );

    expect(onNotify).toHaveBeenCalledTimes(1);
    expect(onNotify).toHaveBeenCalledWith('mesh.event', { node: 7 });
  });
});
