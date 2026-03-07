import { beforeEach, describe, expect, it, vi } from 'vitest';
import { WebSocketTransport } from '../../src/transport/WebSocketTransport';

type Listener = (event?: any) => void;

class MockWebSocket {
  static OPEN = 1;
  static CLOSED = 3;

  readyState = MockWebSocket.OPEN;
  private listeners = new Map<string, Listener[]>();

  constructor(_url: string) {
    queueMicrotask(() => this.emit('open'));
  }

  addEventListener(type: string, cb: Listener): void {
    const list = this.listeners.get(type) ?? [];
    list.push(cb);
    this.listeners.set(type, list);
  }

  removeEventListener(type: string, cb: Listener): void {
    const list = this.listeners.get(type) ?? [];
    this.listeners.set(type, list.filter(l => l !== cb));
  }

  close(): void {
    this.readyState = MockWebSocket.CLOSED;
  }

  send(_data: string): void {}

  emit(type: string, event: any = {}): void {
    for (const cb of this.listeners.get(type) ?? []) cb(event);
  }
}

describe('WebSocketTransport auth close handling', () => {
  beforeEach(() => {
    vi.stubGlobal('WebSocket', MockWebSocket as any);
    Object.defineProperty(document, 'visibilityState', {
      configurable: true,
      get: () => 'visible',
    });
  });

  it('does not schedule auto-reconnect on policy violation close code 1008', async () => {
    const transport = new WebSocketTransport('ws://node/ws');
    await transport.connect();
    transport.enableAutoReconnect();

    const ws = (transport as any).ws as MockWebSocket;
    ws.emit('close', { code: 1008 });

    expect((transport as any).reconnectTimer).toBeNull();
  });
});
