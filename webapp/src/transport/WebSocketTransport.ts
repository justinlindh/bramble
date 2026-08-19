import type { Transport } from '../types/bramble';
import { RpcCorrelation } from './rpcCorrelation';

export interface WsReconnectCallbacks {
  onDisconnect?: () => void;
  onReconnect?: () => void;
}

export class WebSocketTransport implements Transport {
  private ws: WebSocket | null = null;
  private _connected = false;
  private readonly rpc = new RpcCorrelation();
  private notifyCb: ((method: string, params: unknown) => void) | null = null;
  readonly url: string;
  private readonly authToken?: string;

  // Reconnect state
  private autoReconnect = false;
  private reconnectTimer: ReturnType<typeof setTimeout> | null = null;
  private reconnectDelay = 1000;
  private reconnectCbs: WsReconnectCallbacks = {};
  private intentionalClose = false;

  // Keepalive
  private pingTimer: ReturnType<typeof setInterval> | null = null;
  private lastPong = 0;
  private static PING_INTERVAL = 10_000;  // send ping every 10s
  private static PONG_TIMEOUT = 5_000;    // if no pong in 5s, consider dead

  constructor(url: string, authToken?: string) {
    this.url = url;
    this.authToken = authToken;
  }

  get connected(): boolean {
    return this._connected;
  }

  connect(): Promise<void> {
    return new Promise((resolve, reject) => {
      if (this._connected && this.ws?.readyState === WebSocket.OPEN) {
        resolve();
        return;
      }

      // Clean up any stale socket
      this.cleanup();

      const protocols = this.authToken
        ? [`bramble.v1.auth.${this.authToken}`, 'bramble.v1']
        : ['bramble.v1'];
      const ws = new WebSocket(this.url, protocols);
      this.ws = ws;

      const connectTimeout = setTimeout(() => {
        ws.removeEventListener('open', onOpen);
        ws.removeEventListener('error', onInitError);
        try { ws.close(); } catch { /* */ }
        reject(new Error(`WebSocket connect timeout: ${this.url}`));
      }, 8000);

      const onOpen = () => {
        clearTimeout(connectTimeout);
        ws.removeEventListener('error', onInitError);
        this._connected = true;
        this.intentionalClose = false;
        this.lastPong = Date.now();
        this.startKeepalive();
        resolve();
      };

      const onInitError = () => {
        clearTimeout(connectTimeout);
        ws.removeEventListener('open', onOpen);
        reject(new Error(`WebSocket connection failed: ${this.url}`));
      };

      ws.addEventListener('open', onOpen, { once: true });
      ws.addEventListener('error', onInitError, { once: true });

      ws.addEventListener('message', (ev) => {
        this.handleMessage(ev.data);
      });

      ws.addEventListener('close', (event: CloseEvent) => {
        const wasConnected = this._connected;
        const isAuthFailure = event.code === 1008;
        this.cleanup();

        if (!wasConnected) {
          clearTimeout(connectTimeout);
          ws.removeEventListener('open', onOpen);
          ws.removeEventListener('error', onInitError);
          if (isAuthFailure) {
            reject(new Error('Authentication required. Enter your device token.'));
          } else {
            reject(new Error(`WebSocket closed during connect: ${this.url}`));
          }
          return;
        }

        if (wasConnected && !this.intentionalClose) {
          if (isAuthFailure) {
            console.warn('[WS] Authentication rejected (1008); not reconnecting');
            return;
          }
          this.reconnectCbs.onDisconnect?.();
          this.scheduleReconnect();
        }
      });

      ws.addEventListener('error', () => {
        // Error before close: close event will follow and handle reconnect
      });
    });
  }

  private handleMessage(data: string): void {
    // Any message counts as a "pong" for keepalive
    this.lastPong = Date.now();

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

  async sendRPC<T>(method: string, params: Record<string, unknown> = {}, timeoutMs = 5000): Promise<T> {
    if (!this._connected || !this.ws || this.ws.readyState !== WebSocket.OPEN) {
      throw new Error('Not connected');
    }

    const id = this.rpc.nextId();
    try {
      this.ws.send(JSON.stringify({ jsonrpc: '2.0', id, method, params }));
    } catch (e) {
      throw new Error(`WebSocket send failed: ${(e as Error).message}`);
    }

    return this.rpc.request<T>(id, method, timeoutMs);
  }

  onNotification(cb: (method: string, params: unknown) => void): void {
    this.notifyCb = cb;
  }

  async disconnect(): Promise<void> {
    this.intentionalClose = true;
    this.autoReconnect = false;
    this.clearReconnectTimer();
    if (this.visibilityHandler) {
      document.removeEventListener('visibilitychange', this.visibilityHandler);
      this.visibilityHandler = null;
    }
    this.cleanup();
  }

  private visibilityHandler: (() => void) | null = null;

  /** Enable auto-reconnect. Call once after first successful connect(). */
  enableAutoReconnect(cbs?: WsReconnectCallbacks): void {
    this.autoReconnect = true;
    this.reconnectDelay = 1000;
    this.reconnectCbs = cbs ?? {};

    // Immediately check connection when tab becomes visible (mobile resume)
    if (this.visibilityHandler) {
      document.removeEventListener('visibilitychange', this.visibilityHandler);
    }
    this.visibilityHandler = () => {
      if (document.visibilityState !== 'visible') return;
      if (this._connected && this.ws?.readyState === WebSocket.OPEN) {
        // Connection looks alive: send an immediate ping to verify
        try {
          this.ws.send(JSON.stringify({ jsonrpc: '2.0', id: this.rpc.nextId(), method: 'bramble.ping' }));
        } catch { /* will trigger close → reconnect */ }
        // If no response within 2s, force close
        setTimeout(() => {
          if (this._connected && Date.now() - this.lastPong > 3000) {
            console.warn('[WS] Stale after resume: forcing reconnect');
            try { this.ws?.close(); } catch { /* */ }
          }
        }, 2000);
      } else if (this.autoReconnect && !this._connected && !this.reconnectTimer) {
        // Not connected, no pending reconnect: try immediately
        console.log('[WS] Tab visible: immediate reconnect');
        this.reconnectDelay = 500;
        this.scheduleReconnect();
      }
    };
    document.addEventListener('visibilitychange', this.visibilityHandler);
  }

  // ── Internal helpers ──────────────────────────────────────────────

  private cleanup(): void {
    this._connected = false;
    this.stopKeepalive();
    this.rpc.rejectAll(new Error('Connection closed'));
    if (this.ws) {
      try { this.ws.close(); } catch { /* */ }
      this.ws = null;
    }
  }

  private clearReconnectTimer(): void {
    if (this.reconnectTimer) {
      clearTimeout(this.reconnectTimer);
      this.reconnectTimer = null;
    }
  }

  // ── Keepalive ─────────────────────────────────────────────────────

  private startKeepalive(): void {
    this.stopKeepalive();
    this.pingTimer = setInterval(() => {
      if (!this._connected || !this.ws || this.ws.readyState !== WebSocket.OPEN) return;

      // Check if we haven't heard anything in a while
      const silence = Date.now() - this.lastPong;
      if (silence > WebSocketTransport.PING_INTERVAL + WebSocketTransport.PONG_TIMEOUT) {
        console.warn(`[WS] No data in ${(silence / 1000).toFixed(0)}s: closing as dead`);
        try { this.ws.close(); } catch { /* */ }
        return;
      }

      // Send a lightweight ping RPC: any response resets lastPong
      try {
        const pingId = this.rpc.nextId();
        this.ws.send(JSON.stringify({ jsonrpc: '2.0', id: pingId, method: 'bramble.ping' }));
        // Nothing awaits the pong; any inbound message refreshes lastPong. The
        // entry clears itself on response, on disconnect, or after 10s.
        this.rpc.request(pingId, 'bramble.ping', 10_000).catch(() => { /* pong is optional */ });
      } catch {
        // send failed: close event will fire
      }
    }, WebSocketTransport.PING_INTERVAL);
  }

  private stopKeepalive(): void {
    if (this.pingTimer) {
      clearInterval(this.pingTimer);
      this.pingTimer = null;
    }
  }

  // ── Reconnect ─────────────────────────────────────────────────────

  private scheduleReconnect(): void {
    if (!this.autoReconnect || this.reconnectTimer) return;
    const delay = Math.min(this.reconnectDelay, 15000);
    console.log(`[WS] Reconnecting in ${delay}ms…`);
    this.reconnectTimer = setTimeout(async () => {
      this.reconnectTimer = null;
      try {
        await this.connect();
        this.reconnectDelay = 1000;
        console.log('[WS] Reconnected');
        this.reconnectCbs.onReconnect?.();
      } catch {
        this.reconnectDelay = Math.min(this.reconnectDelay * 1.5, 15000);
        this.scheduleReconnect();
      }
    }, delay);
  }
}
