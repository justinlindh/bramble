import type { Transport } from '../types/bramble';

interface Pending {
  resolve: (v: unknown) => void;
  reject: (e: Error) => void;
  timer: ReturnType<typeof setTimeout>;
}

function resolveWsUrl(): string {
  if (typeof location === 'undefined') return 'ws://localhost:3005';
  const { hostname, protocol, port } = location;
  const wsProtocol = protocol === 'https:' ? 'wss:' : 'ws:';

  // When served via Caddy (HTTPS), WebSocket is proxied through /ws path
  if (protocol === 'https:') {
    return `${wsProtocol}//${hostname}:${port || '443'}/ws`;
  }

  // Local dev: direct connection to mock-node
  if (hostname === 'localhost' || hostname === '127.0.0.1') {
    return 'ws://localhost:3005';
  }

  // LAN HTTP: direct to mock-node port
  return `ws://${hostname}:3005`;
}

export class WebSocketTransport implements Transport {
  private ws: WebSocket | null = null;
  private _connected = false;
  private rpcId = 0;
  private pending = new Map<number, Pending>();
  private notifyCb: ((method: string, params: unknown) => void) | null = null;
  private readonly url: string;

  constructor(url?: string) {
    this.url = url ?? resolveWsUrl();
  }

  get connected(): boolean {
    return this._connected;
  }

  connect(): Promise<void> {
    return new Promise((resolve, reject) => {
      if (this._connected) {
        resolve();
        return;
      }

      const ws = new WebSocket(this.url);
      this.ws = ws;

      const onOpen = () => {
        this._connected = true;
        ws.removeEventListener('error', onInitError);
        resolve();
      };

      const onInitError = (ev: Event) => {
        ws.removeEventListener('open', onOpen);
        reject(new Error(`WebSocket connection failed: ${this.url}`));
      };

      ws.addEventListener('open', onOpen, { once: true });
      ws.addEventListener('error', onInitError, { once: true });

      ws.addEventListener('message', (ev) => {
        this.handleMessage(ev.data);
      });

      ws.addEventListener('close', () => {
        if (this._connected) {
          this._connected = false;
          this.rejectAll(new Error('WebSocket connection closed'));
        }
      });

      ws.addEventListener('error', () => {
        if (this._connected) {
          this._connected = false;
          this.rejectAll(new Error('WebSocket error'));
        }
      });
    });
  }

  private handleMessage(data: string): void {
    let msg: Record<string, unknown>;
    try {
      msg = JSON.parse(data);
    } catch {
      return;
    }

    if ('id' in msg && typeof msg.id === 'number' && this.pending.has(msg.id)) {
      const { resolve, reject, timer } = this.pending.get(msg.id)!;
      clearTimeout(timer);
      this.pending.delete(msg.id);
      if (msg.error) {
        reject(new Error((msg.error as { message: string }).message));
      } else {
        resolve(msg.result);
      }
    } else if (msg.method && !('id' in msg)) {
      this.notifyCb?.(msg.method as string, msg.params);
    }
  }

  async sendRPC<T>(method: string, params: Record<string, unknown> = {}, timeoutMs = 5000): Promise<T> {
    if (!this._connected || !this.ws) throw new Error('Not connected');
    const id = ++this.rpcId;
    this.ws.send(JSON.stringify({ jsonrpc: '2.0', id, method, params }));

    return new Promise<T>((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(id);
        reject(new Error(`RPC timeout: ${method}`));
      }, timeoutMs);
      this.pending.set(id, {
        resolve: resolve as (v: unknown) => void,
        reject,
        timer,
      });
    });
  }

  onNotification(cb: (method: string, params: unknown) => void): void {
    this.notifyCb = cb;
  }

  private rejectAll(err: Error): void {
    for (const [, { reject, timer }] of this.pending) {
      clearTimeout(timer);
      reject(err);
    }
    this.pending.clear();
  }

  async disconnect(): Promise<void> {
    this._connected = false;
    this.rejectAll(new Error('Disconnected'));
    try {
      this.ws?.close();
    } catch {
      /* ignore */
    }
    this.ws = null;
  }
}
