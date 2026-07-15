import { beforeEach, describe, expect, it, vi } from 'vitest';
import { WebSocketTransport } from '../../src/transport/WebSocketTransport';
import { FakeWebSocket } from './fakeWebSocket';

describe('WebSocketTransport auth close handling', () => {
  beforeEach(() => {
    FakeWebSocket.instances = [];
    FakeWebSocket.failNextOpens = 0;
    vi.stubGlobal('WebSocket', FakeWebSocket as any);
    Object.defineProperty(document, 'visibilityState', {
      configurable: true,
      get: () => 'visible',
    });
  });

  it('does not schedule auto-reconnect on policy violation close code 1008', async () => {
    const transport = new WebSocketTransport('ws://node/ws');
    await transport.connect();
    transport.enableAutoReconnect();

    const ws = (transport as any).ws as FakeWebSocket;
    ws.serverClose(1008);

    expect((transport as any).reconnectTimer).toBeNull();
  });

  it('offers the auth-token subprotocol plus bramble.v1 when a token is provided', async () => {
    const transport = new WebSocketTransport('ws://node/ws', 'SECRET123');
    await transport.connect();
    expect(((transport as any).ws as FakeWebSocket).protocols)
      .toEqual(['bramble.v1.auth.SECRET123', 'bramble.v1']);
  });

  it('offers only bramble.v1 when no token is provided', async () => {
    const transport = new WebSocketTransport('ws://node/ws');
    await transport.connect();
    expect(((transport as any).ws as FakeWebSocket).protocols).toEqual(['bramble.v1']);
  });
});
