import { afterEach, describe, expect, it, vi } from 'vitest';
import { session } from '../../src/store/actions/client';
import { addChannel, removeChannel } from '../../src/store/actions';
import type { BrambleClient } from '../../src/transport';

// A stub client whose rpc() answers the mutation under test with `result` and
// every other method (the getConfig reload) with an empty config.
function stubClient(mutation: string, result: unknown) {
  const rpc = vi.fn(async (method: string) => (method === mutation ? result : {}));
  session.client = { rpc } as unknown as BrambleClient;
  return rpc;
}

const methodsCalled = (rpc: ReturnType<typeof stubClient>) => rpc.mock.calls.map(([method]) => method);

describe('config mutations', () => {
  afterEach(() => {
    session.client = null;
  });

  it('returns the new channel index and reloads config', async () => {
    const rpc = stubClient('bramble.addChannel', { ok: true, index: 3, name: 'team' });

    await expect(addChannel('team', 'secret')).resolves.toBe(3);
    expect(rpc).toHaveBeenNthCalledWith(1, 'bramble.addChannel', { name: 'team', psk: 'secret' });
    expect(methodsCalled(rpc)).toEqual(['bramble.addChannel', 'bramble.getConfig']);
  });

  it('omits psk when none is given', async () => {
    const rpc = stubClient('bramble.addChannel', { ok: true, index: 1 });

    await addChannel('open');
    expect(rpc).toHaveBeenNthCalledWith(1, 'bramble.addChannel', { name: 'open' });
  });

  it('throws the device error on ok:false and does not reload config', async () => {
    const rpc = stubClient('bramble.addChannel', { ok: false, error: 'channel limit reached' });

    await expect(addChannel('team')).rejects.toThrow('channel limit reached');
    expect(methodsCalled(rpc)).toEqual(['bramble.addChannel']);
  });

  it('falls back to the action message when ok:false carries no error', async () => {
    stubClient('bramble.removeChannel', { ok: false });

    await expect(removeChannel(2)).rejects.toThrow('Failed to remove channel');
  });

  it('throws when a successful addChannel response carries no index', async () => {
    stubClient('bramble.addChannel', { ok: true });

    await expect(addChannel('team')).rejects.toThrow('Failed to add channel');
  });

  it('throws Not connected without a client', async () => {
    await expect(removeChannel(1)).rejects.toThrow('Not connected');
  });
});
