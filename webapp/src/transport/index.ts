import { SerialTransport } from './SerialTransport';
import { BLETransport } from './BLETransport';
import { WebSocketTransport } from './WebSocketTransport';
import type { Transport, TransportType } from '../types/bramble';

export { SerialTransport } from './SerialTransport';
export { BLETransport } from './BLETransport';
export { WebSocketTransport } from './WebSocketTransport';

function resolveMockWsUrl(): string {
  if (typeof location === 'undefined') return 'ws://localhost:3099';
  const { hostname, protocol, port } = location;
  const wsProtocol = protocol === 'https:' ? 'wss:' : 'ws:';
  if (protocol === 'https:') return `${wsProtocol}//${hostname}:${port || '443'}/ws`;
  if (hostname === 'localhost' || hostname === '127.0.0.1') return 'ws://localhost:3099';
  return `ws://${hostname}:3005`;
}

export function createTransport(type: TransportType, options?: { url?: string; token?: string }): Transport {
  if (type === 'ble') return new BLETransport();
  if (type === 'websocket') return new WebSocketTransport(resolveMockWsUrl());
  if (type === 'wifi') return new WebSocketTransport(options?.url ?? 'ws://192.168.4.1/ws', options?.token);
  return new SerialTransport();
}

// Convenience wrapper: handles notifications, multi-subscriber fan-out
export class BrambleClient {
  private transport: Transport;
  private notifySubs = new Map<string, Set<(params: unknown) => void>>();

  constructor(transport: Transport) {
    this.transport = transport;
    transport.onNotification((method, params) => {
      this.notifySubs.get(method)?.forEach(fn => fn(params));
    });
  }

  get connected(): boolean {
    return this.transport.connected;
  }

  async connect(): Promise<void> {
    return this.transport.connect();
  }

  async disconnect(): Promise<void> {
    return this.transport.disconnect();
  }

  async rpc<T>(method: string, params?: Record<string, unknown>, timeoutMs?: number): Promise<T> {
    return this.transport.sendRPC<T>(method, params, timeoutMs);
  }

  subscribe(method: string, cb: (params: unknown) => void): () => void {
    if (!this.notifySubs.has(method)) {
      this.notifySubs.set(method, new Set());
    }
    this.notifySubs.get(method)!.add(cb);
    return () => this.notifySubs.get(method)?.delete(cb);
  }

  clearSubscriptions(): void {
    this.notifySubs.clear();
  }
}
