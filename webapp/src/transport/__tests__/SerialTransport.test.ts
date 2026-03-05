import { describe, expect, it, vi } from 'vitest';
import { SerialTransport } from '../SerialTransport';

describe('SerialTransport serial JSON-RPC parsing (regressions)', () => {
  function setupConnectedTransport() {
    const transport = new SerialTransport();
    const writer = { write: vi.fn().mockResolvedValue(undefined) };

    (transport as any)._connected = true;
    (transport as any).writer = writer;

    return { transport, writer };
  }

  function feedChunk(transport: SerialTransport, chunk: string) {
    (transport as any).lineBuf += chunk;
    (transport as any).processLines();
  }

  it('should parse JSON-RPC response even when log prefix appears before JSON on the same line', async () => {
    const { transport } = setupConnectedTransport();

    const rpcPromise = transport.sendRPC<{ ok: boolean }>('ping', {}, 50);

    feedChunk(
      transport,
      'I (1204) serial: tx complete {"jsonrpc":"2.0","id":1,"result":{"ok":true}}\n',
    );

    await expect(rpcPromise).resolves.toEqual({ ok: true });
  });

  it('should reconstruct and parse JSON-RPC response split across chunks with an internal newline', async () => {
    const { transport } = setupConnectedTransport();

    const rpcPromise = transport.sendRPC<{ ok: boolean }>('ping', {}, 50);

    feedChunk(transport, '{"jsonrpc":"2.0","id":1,\n');
    feedChunk(transport, '"result":{"ok":true}}\n');

    await expect(rpcPromise).resolves.toEqual({ ok: true });
  });
});
