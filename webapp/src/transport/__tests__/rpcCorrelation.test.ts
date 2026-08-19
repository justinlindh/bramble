import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { RpcCorrelation } from '../rpcCorrelation';

describe('RpcCorrelation', () => {
  beforeEach(() => {
    vi.useFakeTimers();
  });
  afterEach(() => {
    vi.useRealTimers();
  });

  it('hands out monotonic ids', () => {
    const rpc = new RpcCorrelation();
    expect(rpc.nextId()).toBe(1);
    expect(rpc.nextId()).toBe(2);
    expect(rpc.nextId()).toBe(3);
  });

  it('settles a request with its result', async () => {
    const rpc = new RpcCorrelation();
    const id = rpc.nextId();
    const p = rpc.request<{ pong: boolean }>(id, 'bramble.ping', 1000);
    expect(rpc.settle({ id, result: { pong: true } })).toBe(true);
    await expect(p).resolves.toEqual({ pong: true });
  });

  it('rejects a request carrying an error', async () => {
    const rpc = new RpcCorrelation();
    const id = rpc.nextId();
    const p = rpc.request(id, 'bramble.getStatus', 1000);
    expect(rpc.settle({ id, error: { message: 'boom' } })).toBe(true);
    await expect(p).rejects.toThrow('boom');
  });

  it('rejects with an RPC timeout when no response arrives', async () => {
    const rpc = new RpcCorrelation();
    const id = rpc.nextId();
    const p = rpc.request(id, 'bramble.getStatus', 1000);
    const assertion = expect(p).rejects.toThrow('RPC timeout: bramble.getStatus');
    await vi.advanceTimersByTimeAsync(1000);
    await assertion;
    // The timed-out entry is gone: a late response is ignored.
    expect(rpc.settle({ id, result: {} })).toBe(false);
  });

  it('ignores messages that match no tracked id', () => {
    const rpc = new RpcCorrelation();
    expect(rpc.settle({ id: 999, result: {} })).toBe(false);
    expect(rpc.settle({ method: 'bramble.event', params: {} })).toBe(false);
  });

  it('fails every outstanding request when the link drops', async () => {
    const rpc = new RpcCorrelation();
    const a = rpc.request(rpc.nextId(), 'a', 5000);
    const b = rpc.request(rpc.nextId(), 'b', 5000);
    const aRej = expect(a).rejects.toThrow('gone');
    const bRej = expect(b).rejects.toThrow('gone');
    rpc.rejectAll(new Error('gone'));
    await aRej;
    await bRej;
  });

  it('tracks a self-managing entry and forgets it without settling', () => {
    const rpc = new RpcCorrelation();
    const id = rpc.nextId();
    const resolve = vi.fn();
    const reject = vi.fn();
    rpc.track(id, { resolve, reject, timer: setTimeout(() => {}, 0) });
    rpc.forget(id);
    // Forgotten: a later response matches nothing and the handlers never run.
    expect(rpc.settle({ id, result: {} })).toBe(false);
    expect(resolve).not.toHaveBeenCalled();
    expect(reject).not.toHaveBeenCalled();
  });
});
