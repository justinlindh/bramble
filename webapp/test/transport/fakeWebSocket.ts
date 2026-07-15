// Minimal fake WebSocket: mirrors the real connect/open/close/message
// lifecycle (readyState transitions, addEventListener/removeEventListener,
// send()) so WebSocketTransport's actual framing, keepalive, and reconnect
// logic runs unmodified against it, the same approach the BLE harness uses
// for BLETransport. Shared by every WebSocketTransport test file.
type Listener = (event: any) => void;

export class FakeWebSocket {
  static CONNECTING = 0;
  static OPEN = 1;
  static CLOSING = 2;
  static CLOSED = 3;
  static instances: FakeWebSocket[] = [];
  /** When > 0, the next N constructed sockets fail (close) instead of
   * opening, decremented per construction. Models an unreachable node. */
  static failNextOpens = 0;

  readyState = FakeWebSocket.CONNECTING;
  sent: string[] = [];
  url: string;
  protocols?: string[];
  private listeners = new Map<string, Listener[]>();

  constructor(url: string, protocols?: string[]) {
    this.url = url;
    this.protocols = protocols;
    FakeWebSocket.instances.push(this);
    const shouldFail = FakeWebSocket.failNextOpens > 0;
    if (shouldFail) FakeWebSocket.failNextOpens -= 1;
    // Real WebSockets open (or fail) asynchronously; a microtask mirrors
    // that without depending on fake timers (queueMicrotask is not affected
    // by them).
    queueMicrotask(() => {
      if (this.readyState !== FakeWebSocket.CONNECTING) return;
      if (shouldFail) {
        this.readyState = FakeWebSocket.CLOSED;
        this.emit('close', { code: 1006 });
        return;
      }
      this.readyState = FakeWebSocket.OPEN;
      this.emit('open');
    });
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

  send(data: string): void {
    if (this.readyState !== FakeWebSocket.OPEN) {
      throw new Error('FakeWebSocket: send() while not open');
    }
    this.sent.push(data);
  }

  close(): void {
    if (this.readyState === FakeWebSocket.CLOSED) return;
    this.readyState = FakeWebSocket.CLOSED;
    this.emit('close', { code: 1000 });
  }

  private emit(type: string, event: any = {}): void {
    for (const cb of this.listeners.get(type) ?? []) cb(event);
  }

  /** Simulate the node pushing a JSON-RPC response or notification line. */
  serverSend(payload: unknown): void {
    this.emit('message', { data: JSON.stringify(payload) });
  }

  /** Simulate a remote/abnormal close (dead link, node reboot, auth reject). */
  serverClose(code: number): void {
    if (this.readyState === FakeWebSocket.CLOSED) return;
    this.readyState = FakeWebSocket.CLOSED;
    this.emit('close', { code });
  }

  lastSentFrame(): any {
    return JSON.parse(this.sent[this.sent.length - 1]);
  }
}

export function latestSocket(): FakeWebSocket {
  const ws = FakeWebSocket.instances[FakeWebSocket.instances.length - 1];
  if (!ws) throw new Error('no FakeWebSocket instance was constructed');
  return ws;
}
