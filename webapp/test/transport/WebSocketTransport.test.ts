import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { WebSocketTransport } from '../../src/transport/WebSocketTransport';
import { FakeWebSocket, latestSocket } from './fakeWebSocket';

/** Connect and let the FakeWebSocket's queued 'open' microtask settle,
 * whether or not fake timers are active (advanceTimersByTimeAsync(0) also
 * drains the microtask queue). */
async function connectAndOpen(transport: WebSocketTransport): Promise<FakeWebSocket> {
  const p = transport.connect();
  await vi.advanceTimersByTimeAsync(0);
  await p;
  return latestSocket();
}

describe('WebSocketTransport', () => {
  beforeEach(() => {
    FakeWebSocket.instances = [];
    FakeWebSocket.failNextOpens = 0;
    vi.stubGlobal('WebSocket', FakeWebSocket as any);
  });

  afterEach(() => {
    vi.unstubAllGlobals();
    vi.useRealTimers();
  });

  describe('RPC framing', () => {
    it('frames sendRPC as a JSON-RPC 2.0 line and resolves on the matching id', async () => {
      const transport = new WebSocketTransport('ws://node/ws');
      await transport.connect();
      const ws = latestSocket();

      const rpcPromise = transport.sendRPC<{ ok: boolean }>('bramble.getStatus', { verbose: true }, 1000);

      expect(ws.sent).toHaveLength(1);
      const frame = ws.lastSentFrame();
      expect(frame).toMatchObject({ jsonrpc: '2.0', method: 'bramble.getStatus', params: { verbose: true } });
      expect(typeof frame.id).toBe('number');

      ws.serverSend({ jsonrpc: '2.0', id: frame.id, result: { ok: true } });

      await expect(rpcPromise).resolves.toEqual({ ok: true });
    });

    it('rejects sendRPC when the response carries an error object', async () => {
      const transport = new WebSocketTransport('ws://node/ws');
      await transport.connect();
      const ws = latestSocket();

      const rpcPromise = transport.sendRPC('bramble.sendMessage', {}, 1000);
      const frame = ws.lastSentFrame();
      ws.serverSend({ jsonrpc: '2.0', id: frame.id, error: { code: -1, message: 'boom' } });

      await expect(rpcPromise).rejects.toThrow('boom');
    });

    it('ignores a response whose id does not match any pending RPC', async () => {
      const transport = new WebSocketTransport('ws://node/ws');
      await transport.connect();
      const ws = latestSocket();

      const rpcPromise = transport.sendRPC<{ ok: boolean }>('bramble.getStatus', {}, 1000);
      const frame = ws.lastSentFrame();

      // A stray response for an id nobody is waiting on must not resolve or
      // throw; the real response (matching id) still lands afterward.
      ws.serverSend({ jsonrpc: '2.0', id: frame.id + 999, result: { ok: false } });
      ws.serverSend({ jsonrpc: '2.0', id: frame.id, result: { ok: true } });

      await expect(rpcPromise).resolves.toEqual({ ok: true });
    });

    it('routes a notification (a message with no id) to the notification handler', async () => {
      const transport = new WebSocketTransport('ws://node/ws');
      await transport.connect();
      const ws = latestSocket();

      const notifyCb = vi.fn();
      transport.onNotification(notifyCb);

      ws.serverSend({ jsonrpc: '2.0', method: 'bramble.onAck', params: { packetId: 'pkt-1', status: 'delivered' } });

      expect(notifyCb).toHaveBeenCalledWith('bramble.onAck', { packetId: 'pkt-1', status: 'delivered' });
    });
  });

  describe('keepalive', () => {
    it('sends a bramble.ping on the keepalive interval while connected', async () => {
      vi.useFakeTimers();
      const transport = new WebSocketTransport('ws://node/ws');
      const ws = await connectAndOpen(transport);
      expect(ws.sent).toHaveLength(0);

      // WebSocketTransport.PING_INTERVAL is 10_000ms (private).
      await vi.advanceTimersByTimeAsync(10_000);

      expect(ws.sent).toHaveLength(1);
      expect(ws.lastSentFrame()).toMatchObject({ jsonrpc: '2.0', method: 'bramble.ping' });
    });

    it('keeps pinging on every subsequent interval as long as the node answers', async () => {
      vi.useFakeTimers();
      const transport = new WebSocketTransport('ws://node/ws');
      const ws = await connectAndOpen(transport);

      await vi.advanceTimersByTimeAsync(10_000);
      expect(ws.sent).toHaveLength(1);
      // Any inbound message (not just a ping reply) resets the keepalive
      // clock, same as a real node's traffic would: answer the ping so the
      // second interval pings again instead of declaring the link dead.
      ws.serverSend({ jsonrpc: '2.0', id: ws.lastSentFrame().id, result: {} });

      await vi.advanceTimersByTimeAsync(10_000);

      expect(ws.sent).toHaveLength(2);
      expect(transport.connected).toBe(true);
    });

    it('closes the link as dead when no data arrives for PING_INTERVAL + PONG_TIMEOUT', async () => {
      vi.useFakeTimers();
      const transport = new WebSocketTransport('ws://node/ws');
      await connectAndOpen(transport);

      // First interval: pings, but nothing answers it.
      await vi.advanceTimersByTimeAsync(10_000);
      expect(transport.connected).toBe(true);
      // Second interval: 20s of silence exceeds PING_INTERVAL (10s) +
      // PONG_TIMEOUT (5s), so the transport gives up on the link.
      await vi.advanceTimersByTimeAsync(10_000);

      expect(transport.connected).toBe(false);
    });
  });

  describe('auto-reconnect', () => {
    it('retries with backoff after an unexpected close and fires onReconnect once reopened', async () => {
      vi.useFakeTimers();
      const transport = new WebSocketTransport('ws://node/ws');
      const ws1 = await connectAndOpen(transport);

      const onDisconnect = vi.fn();
      const onReconnect = vi.fn();
      transport.enableAutoReconnect({ onDisconnect, onReconnect });

      // Abnormal closure (not the auth-reject code 1008, not our own
      // disconnect()): must trigger onDisconnect and schedule a retry.
      ws1.serverClose(1006);
      expect(transport.connected).toBe(false);
      expect(onDisconnect).toHaveBeenCalledTimes(1);

      // First retry fires after the initial 1s backoff and opens a fresh socket.
      await vi.advanceTimersByTimeAsync(1000);
      await vi.advanceTimersByTimeAsync(0);

      const ws2 = latestSocket();
      expect(ws2).not.toBe(ws1);
      expect(transport.connected).toBe(true);
      expect(onReconnect).toHaveBeenCalledTimes(1);
    });

    it('keeps retrying with growing backoff while the node stays unreachable', async () => {
      vi.useFakeTimers();
      const transport = new WebSocketTransport('ws://node/ws');
      const ws1 = await connectAndOpen(transport);
      transport.enableAutoReconnect({});

      // Every reconnect attempt fails to even open (out of range / node
      // down): the next two constructed sockets close before 'open'.
      FakeWebSocket.failNextOpens = 2;
      ws1.serverClose(1006);
      expect(FakeWebSocket.instances).toHaveLength(1);

      // First retry fires after the initial 1s backoff and fails to open.
      await vi.advanceTimersByTimeAsync(1000);
      await vi.advanceTimersByTimeAsync(0);
      expect(FakeWebSocket.instances).toHaveLength(2);
      expect(transport.connected).toBe(false);

      // Backoff grew (delay *= 1.5 up to 15s cap): give it enough headroom
      // to fire the next attempt, which also fails to open.
      await vi.advanceTimersByTimeAsync(2000);
      await vi.advanceTimersByTimeAsync(0);
      expect(FakeWebSocket.instances).toHaveLength(3);
      expect(transport.connected).toBe(false);

      // Node comes back: the next attempt succeeds.
      await vi.advanceTimersByTimeAsync(4000);
      await vi.advanceTimersByTimeAsync(0);
      expect(FakeWebSocket.instances.length).toBeGreaterThanOrEqual(4);
      expect(transport.connected).toBe(true);
    });

    it('does not schedule a reconnect on an auth-rejection close (code 1008)', async () => {
      vi.useFakeTimers();
      const transport = new WebSocketTransport('ws://node/ws', 'bad-token');
      const ws = await connectAndOpen(transport);
      transport.enableAutoReconnect({});

      ws.serverClose(1008);

      await vi.advanceTimersByTimeAsync(20_000);
      expect(FakeWebSocket.instances).toHaveLength(1);
    });

    it('does not attempt to reconnect after an intentional disconnect()', async () => {
      vi.useFakeTimers();
      const transport = new WebSocketTransport('ws://node/ws');
      await connectAndOpen(transport);
      const onDisconnect = vi.fn();
      transport.enableAutoReconnect({ onDisconnect });

      const instancesBefore = FakeWebSocket.instances.length;
      await transport.disconnect();

      // Well past the initial 1s backoff: no new socket should ever open.
      await vi.advanceTimersByTimeAsync(20_000);

      expect(onDisconnect).not.toHaveBeenCalled();
      expect(FakeWebSocket.instances.length).toBe(instancesBefore);
    });
  });

  describe('pending RPC cleanup', () => {
    it('rejects all pending RPCs when the socket closes', async () => {
      const transport = new WebSocketTransport('ws://node/ws');
      await transport.connect();
      const ws = latestSocket();

      const rpc1 = transport.sendRPC('bramble.getStatus', {}, 5000);
      const rpc2 = transport.sendRPC('bramble.getNeighbors', {}, 5000);

      ws.serverClose(1006);

      await expect(rpc1).rejects.toThrow(/connection closed/i);
      await expect(rpc2).rejects.toThrow(/connection closed/i);
    });
  });
});
