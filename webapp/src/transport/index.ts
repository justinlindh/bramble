import { SerialTransport } from './SerialTransport';
import { BLETransport } from './BLETransport';
import { WebSocketTransport } from './WebSocketTransport';
import { MockTransport } from './MockTransport';
import { isEmbeddedShell } from '../utils/platform';
import type { Transport, TransportType } from '../types/bramble';
import type { RpcMethod, RpcParams, RpcResult } from '../types/rpc';

export { WebSocketTransport } from './WebSocketTransport';
export { MockTransport } from './MockTransport';

function resolveMockWsUrl(): string {
  if (typeof location === 'undefined') return 'ws://localhost:3099';
  const { hostname, protocol, port } = location;
  const wsProtocol = protocol === 'https:' ? 'wss:' : 'ws:';
  if (protocol === 'https:') return `${wsProtocol}//${hostname}:${port || '443'}/ws`;
  if (hostname === 'localhost' || hostname === '127.0.0.1') return 'ws://localhost:3099';
  return `ws://${hostname}:3005`;
}

export function createTransport(type: TransportType, options?: { url?: string; token?: string; bleDevice?: BluetoothDevice }): Transport {
  if (type === 'ble') return new BLETransport(options?.token, options?.bleDevice);
  if (type === 'websocket') {
    // Embedded shells (Android WebView, Electron under file://) load the app
    // from a local origin with no mock WebSocket server reachable behind it
    // (resolveMockWsUrl() would build e.g. wss://appassets.androidplatform.net:443/ws,
    // which has nothing listening). Drive the mock handler in page instead.
    if (isEmbeddedShell()) return new MockTransport();
    return new WebSocketTransport(resolveMockWsUrl());
  }
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

  // Contract-typed overload: when the method string is one api/openapi.yaml
  // defines (via types/rpcContract.generated.ts), params and result resolve
  // from the contract. The generic overload remains for call sites that
  // intentionally send params outside the contract, such as snake_case
  // fallbacks for older firmware.
  async rpc<M extends RpcMethod>(method: M, params?: RpcParams<M>, timeoutMs?: number): Promise<RpcResult<M>>;
  async rpc<T>(method: string, params?: Record<string, unknown>, timeoutMs?: number): Promise<T>;
  async rpc(method: string, params?: Record<string, unknown>, timeoutMs?: number): Promise<unknown> {
    return this.transport.sendRPC(method, params, timeoutMs);
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
