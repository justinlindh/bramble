import type { Transport } from '../types/bramble';

// Espressif and common USB-serial bridge VIDs
const FILTERS = [
  { usbVendorId: 0x303A }, // Espressif native USB
  { usbVendorId: 0x10C4 }, // CP2102 (Silicon Labs)
  { usbVendorId: 0x1A86 }, // CH340
  { usbVendorId: 0x0403 }, // FTDI
];

interface Pending {
  resolve: (v: unknown) => void;
  reject: (e: Error) => void;
  timer: ReturnType<typeof setTimeout>;
}

export class SerialTransport implements Transport {
  private port: SerialPort | null = null;
  private reader: ReadableStreamDefaultReader<Uint8Array> | null = null;
  private writer: WritableStreamDefaultWriter<Uint8Array> | null = null;
  private _connected = false;
  private rpcId = 0;
  private pending = new Map<number, Pending>();
  private notifyCb: ((method: string, params: unknown) => void) | null = null;
  private lineBuf = '';
  private readonly decoder = new TextDecoder();
  private readonly encoder = new TextEncoder();

  get connected() { return this._connected; }

  async connect(baudRate = 115200): Promise<void> {
    const serial = (navigator as { serial?: { requestPort(opts?: { filters?: { usbVendorId: number }[] }): Promise<SerialPort> } }).serial;
    if (!serial) throw new Error('Web Serial API not supported');
    this.port = await serial.requestPort({ filters: FILTERS });
    await this.port.open({ baudRate });
    this.writer = this.port.writable!.getWriter();
    this.reader = this.port.readable!.getReader();
    this._connected = true;
    this.startReadLoop();
  }

  private startReadLoop(): void {
    (async () => {
      try {
        while (this._connected) {
          const { value, done } = await this.reader!.read();
          if (done) break;
          this.lineBuf += this.decoder.decode(value, { stream: true });
          this.processLines();
        }
      } catch {
        /* port closed */
        if (this._connected) {
          this._connected = false;
          this.rejectAll(new Error('Serial port disconnected'));
        }
      }
    })();
  }

  private processLines(): void {
    const lines = this.lineBuf.split('\n');
    this.lineBuf = lines.pop() ?? '';
    for (const raw of lines) {
      const line = raw.trim();
      if (!line) continue;
      let msg: Record<string, unknown>;
      try { msg = JSON.parse(line); } catch { continue; }

      if ('id' in msg && typeof msg.id === 'number' && this.pending.has(msg.id)) {
        const { resolve, reject, timer } = this.pending.get(msg.id)!;
        clearTimeout(timer);
        this.pending.delete(msg.id);
        if (msg.error) reject(new Error((msg.error as { message: string }).message));
        else resolve(msg.result);
      } else if (msg.method && !('id' in msg)) {
        this.notifyCb?.(msg.method as string, msg.params);
      }
    }
  }

  async sendRPC<T>(method: string, params: Record<string, unknown> = {}, timeoutMs = 5000): Promise<T> {
    if (!this._connected) throw new Error('Not connected');
    const id = ++this.rpcId;
    const payload = this.encoder.encode(
      JSON.stringify({ jsonrpc: '2.0', id, method, params }) + '\n'
    );

    // Register pending BEFORE the async write so disconnect() can reject it
    // even if we're awaiting the write when disconnect fires.
    const promise = new Promise<T>((resolve, reject) => {
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

    try {
      await this.writer!.write(payload);
    } catch {
      // Write failed (port closed) — clean up pending entry
      const entry = this.pending.get(id);
      if (entry) {
        clearTimeout(entry.timer);
        this.pending.delete(id);
        entry.reject(new Error('Not connected'));
      }
    }

    return promise;
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
    try { await this.reader?.cancel(); this.reader?.releaseLock(); } catch { /* ignore */ }
    try { this.writer?.releaseLock(); } catch { /* ignore */ }
    try { await this.port?.close(); } catch { /* ignore */ }
    this.reader = null;
    this.writer = null;
    this.port = null;
  }
}
