import type { Transport } from '../types/bramble';
import { handleConnection } from '../../mock/handler.mjs';
import type { MockSocketLike, MockRequestLike } from '../../mock/handler.mjs';
import { RpcCorrelation } from './rpcCorrelation';

/**
 * In-page mock transport for embedded shells (Android WebView, Electron
 * under file://). Those shells load the webapp from a local origin with no
 * mock WebSocket server behind it, so resolveMockWsUrl()'s location-derived
 * URL has nothing to connect to. This transport drives mock/handler.mjs's
 * RPC logic in process instead, using a fake ws-like socket as the adapter
 * between handler.mjs's `ws.send` / `ws.on('message'|'close'|'error')`
 * contract and this transport's RPC-response / notification plumbing.
 *
 * Mirrors WebSocketTransport's message shapes exactly (same JSON-RPC 2.0
 * request/response/notification framing) since both sides speak to the same
 * handler.mjs. No network, no timers for connect: connect() resolves
 * immediately.
 */
export class MockTransport implements Transport {
  private _connected = false;
  private readonly rpc = new RpcCorrelation();
  private notifyCb: ((method: string, params: unknown) => void) | null = null;
  private fakeWs: MockSocketLike | null = null;
  private messageListener: ((data: string) => void) | null = null;
  private closeListener: (() => void) | null = null;

  get connected(): boolean {
    return this._connected;
  }

  async connect(): Promise<void> {
    if (this._connected) return;

    const listeners: {
      message?: (data: string) => void;
      close?: () => void;
      error?: (err: Error) => void;
    } = {};

    const fakeWs: MockSocketLike = {
      readyState: 1, // mirrors ws.WebSocket.OPEN
      send: (data: string) => this.handleServerMessage(data),
      close: (_code?: number, _reason?: string) => {
        fakeWs.readyState = 3; // mirrors ws.WebSocket.CLOSED
        listeners.close?.();
      },
      on: (event, listener) => {
        if (event === 'message') listeners.message = listener as (data: string) => void;
        else if (event === 'close') listeners.close = listener as () => void;
        else if (event === 'error') listeners.error = listener as (err: Error) => void;
      },
    };

    const fakeReq: MockRequestLike = {
      url: '/ws',
      socket: { remoteAddress: 'in-page-mock' },
    };

    handleConnection(fakeWs, fakeReq);

    this.fakeWs = fakeWs;
    this.messageListener = listeners.message ?? null;
    this.closeListener = listeners.close ?? null;
    this._connected = true;
  }

  async sendRPC<T>(method: string, params: Record<string, unknown> = {}, timeoutMs = 5000): Promise<T> {
    if (!this._connected || !this.messageListener) {
      throw new Error('Not connected');
    }

    const id = this.rpc.nextId();
    const request = { jsonrpc: '2.0', id, method, params };

    // Register before dispatching: handler.mjs answers synchronously through
    // fakeWs.send, so the response can arrive before this call returns.
    const response = this.rpc.request<T>(id, method, timeoutMs);
    try {
      this.messageListener(JSON.stringify(request));
    } catch (e) {
      // A no-op if the handler already answered before throwing.
      this.rpc.settle({ id, error: { message: `Mock RPC dispatch failed: ${(e as Error).message}` } });
    }
    return response;
  }

  onNotification(cb: (method: string, params: unknown) => void): void {
    this.notifyCb = cb;
  }

  async disconnect(): Promise<void> {
    if (!this._connected) return;
    this._connected = false;
    this.rpc.rejectAll(new Error('Disconnected'));
    try { this.fakeWs?.close(1000, 'client disconnect'); } catch { /* ignore */ }
    this.fakeWs = null;
    this.messageListener = null;
    this.closeListener = null;
  }

  private handleServerMessage(data: string): void {
    let msg: Record<string, unknown>;
    try {
      msg = JSON.parse(data);
    } catch {
      return;
    }

    // RPC response
    if (this.rpc.settle(msg)) return;

    // Notification (no id)
    if (msg.method && !('id' in msg)) {
      this.notifyCb?.(msg.method as string, msg.params);
    }
  }
}
