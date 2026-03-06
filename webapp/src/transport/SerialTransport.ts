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
  method: string;
}

export class SerialTransport implements Transport {
  private port: SerialPort | null = null;
  private reader: ReadableStreamDefaultReader<Uint8Array> | null = null;
  private writer: WritableStreamDefaultWriter<Uint8Array> | null = null;
  private _connected = false;
  private rpcId = 0;
  private pending = new Map<number, Pending>();
  private notifyCb: ((method: string, params: unknown) => void) | null = null;
  private readBuf = '';
  private rxTotal = 0; // monotonic byte counter for drain silence detection
  private readonly decoder = new TextDecoder();
  private readonly encoder = new TextEncoder();
  private static readonly MAX_BUFFER_LENGTH = 64 * 1024;
  // Serialize writes: firmware CLI uses linenoise (interactive line editor)
  // which can only process one command at a time. Bytes arriving while
  // linenoise is in rpc_dispatch() get buffered and mishandled on next read.
  private writeQueue: Promise<void> = Promise.resolve();

  private log(tag: string, data?: Record<string, unknown>): void {
    console.debug(`[serial:${tag}]`, data ?? '');
  }

  get connected() { return this._connected; }

  async connect(baudRate = 115200): Promise<void> {
    const serial = (navigator as { serial?: { requestPort(opts?: { filters?: { usbVendorId: number }[] }): Promise<SerialPort> } }).serial;
    if (!serial) throw new Error('Web Serial API not supported');
    this.port = await serial.requestPort({ filters: FILTERS });
    await this.port.open({ baudRate });

    // Assert DTR for USB-UART bridges (CP2102 etc.) — needed for data flow.
    // Native USB-JTAG may not support signals; best-effort only.
    try {
      const portWithSignals = this.port as unknown as { setSignals?: (signals: { dataTerminalReady?: boolean; requestToSend?: boolean }) => Promise<void> };
      await portWithSignals.setSignals?.({ dataTerminalReady: true, requestToSend: false });
      this.log('dtr-set');
    } catch {
      this.log('dtr-skip', { reason: 'not supported (USB-JTAG)' });
    }

    this.writer = this.port.writable!.getWriter();
    this.reader = this.port.readable!.getReader();
    this._connected = true;

    this.startReadLoop();
    await this.drainStaleData();
  }

  /**
   * Drain stale data from firmware's UART RX buffer after connect.
   *
   * Previous web sessions may have disconnected while RPCs were queued.
   * Those commands sit in the UART buffer and the firmware processes them
   * one by one through linenoise (echo, "Unknown command", prompt).
   * Our new requests queue behind all that stale data and time out.
   *
   * Strategy: send newlines to flush partial lines, then wait until the
   * firmware shows a stable `bramble>` prompt (meaning it's idle).
   */
  private async drainStaleData(): Promise<void> {
    // Send a newline to nudge firmware to a prompt if it's waiting for input.
    try {
      await this.writer!.write(this.encoder.encode('\n'));
    } catch {
      return;
    }

    const startMs = Date.now();
    const deadline = startMs + 10000;
    let lastRxTotal = this.rxTotal;
    let silentSinceMs = Date.now();

    while (Date.now() < deadline) {
      await new Promise(r => setTimeout(r, 50));

      // Use monotonic byte counter — immune to processBuffer() mutations
      if (this.rxTotal !== lastRxTotal) {
        lastRxTotal = this.rxTotal;
        silentSinceMs = Date.now();
        continue;
      }

      const silentMs = Date.now() - silentSinceMs;

      // Test environments (no firmware): no bytes ever arrived
      if (this.rxTotal === 0 && silentMs >= 150) {
        this.readBuf = '';
        this.log('drain-idle', { ms: Date.now() - startMs });
        return;
      }

      // Real firmware: 500ms of no new bytes → stale buffer is drained
      if (silentMs >= 500) {
        this.readBuf = '';
        this.log('drain-done', { ms: Date.now() - startMs, rxTotal: this.rxTotal });
        return;
      }
    }

    // Timeout — clear buffer and proceed anyway
    this.log('drain-timeout', { ms: Date.now() - startMs, rxTotal: this.rxTotal });
    this.readBuf = '';
  }

  private startReadLoop(): void {
    (async () => {
      try {
        while (this._connected) {
          const { value, done } = await this.reader!.read();
          if (done) break;
          const chunk = this.decoder.decode(value, { stream: true });
          this.rxTotal += chunk.length;
          this.readBuf += chunk;
          this.processBuffer();
        }
      } catch (error) {
        if (this._connected) {
          console.warn('[serial] read loop error:', (error as Error)?.message);
          this._connected = false;
          this.rejectAll(new Error('Serial port disconnected'));
        }
      }
    })();
  }

  /**
   * Extract complete JSON objects from the stream buffer using brace-depth
   * tracking, then dispatch them. This is the core event-driven mechanism:
   * every byte that arrives triggers this, and the moment a complete JSON-RPC
   * response is reassembled, the matching pending request resolves immediately.
   */
  private processBuffer(): void {
    const { messages, remainder } = this.extractJsonObjects(this.readBuf);
    this.readBuf = remainder;

    for (const msg of messages) {
      this.dispatchMessage(msg);
    }
  }

  private dispatchMessage(msg: Record<string, unknown>): void {
    // RPC response: has numeric id + result or error
    if ('id' in msg && typeof msg.id === 'number' && this.pending.has(msg.id)) {
      // Ignore echoed requests (have method+id but no result/error)
      if (!('result' in msg) && !('error' in msg)) return;

      const entry = this.pending.get(msg.id)!;
      this.pending.delete(msg.id);

      if (msg.error) {
        entry.reject(new Error((msg.error as { message?: string }).message ?? 'RPC error'));
      } else {
        entry.resolve(msg.result);
      }
      return;
    }

    // Notification: has method but no id
    if (typeof msg.method === 'string' && !('id' in msg)) {
      this.notifyCb?.(msg.method, msg.params);
    }
  }

  /**
   * Brace-depth JSON extractor. Handles:
   * - Fragmented delivery (1-12 byte chunks from CP2102)
   * - Multiple JSON objects in one chunk
   * - Interleaved firmware log noise between JSON objects
   * - Strings with escaped characters and nested braces
   */
  private extractJsonObjects(input: string): { messages: Record<string, unknown>[]; remainder: string } {
    const messages: Record<string, unknown>[] = [];
    let cursor = 0;
    let objectStart = -1;
    let depth = 0;
    let inString = false;
    let escaped = false;

    for (let i = 0; i < input.length; i++) {
      const ch = input[i];

      if (objectStart < 0) {
        if (ch === '{') {
          objectStart = i;
          depth = 1;
          inString = false;
          escaped = false;
        }
        continue;
      }

      if (inString) {
        if (escaped) {
          escaped = false;
        } else if (ch === '\\') {
          escaped = true;
        } else if (ch === '"') {
          inString = false;
        }
        continue;
      }

      if (ch === '"') {
        inString = true;
        continue;
      }

      if (ch === '{') {
        depth++;
      } else if (ch === '}') {
        depth--;
        if (depth === 0) {
          const candidate = input.slice(objectStart, i + 1);
          try {
            const parsed = JSON.parse(candidate) as unknown;
            if (parsed && typeof parsed === 'object' && !Array.isArray(parsed)) {
              messages.push(parsed as Record<string, unknown>);
              cursor = i + 1;
            } else {
              cursor = objectStart + 1;
            }
          } catch {
            cursor = objectStart + 1;
          }

          objectStart = -1;
          depth = 0;
          inString = false;
          escaped = false;
        }
      }
    }

    // Remainder: either mid-object (keep from objectStart) or trailing non-JSON
    let remainder = objectStart >= 0 ? input.slice(objectStart) : input.slice(cursor);

    // Safety valve: if buffer grows huge, trim to last valid start
    if (remainder.length > SerialTransport.MAX_BUFFER_LENGTH) {
      const trimmed = remainder.slice(-SerialTransport.MAX_BUFFER_LENGTH);
      const firstBrace = trimmed.indexOf('{');
      remainder = firstBrace >= 0 ? trimmed.slice(firstBrace) : '';
    }

    return { messages, remainder };
  }

  async sendRPC<T>(method: string, params: Record<string, unknown> = {}, timeoutMs = 5000): Promise<T> {
    if (!this._connected) throw new Error('Not connected');

    const id = ++this.rpcId;
    const payload = this.encoder.encode(
      JSON.stringify({ jsonrpc: '2.0', id, method, params }) + '\r\n'
    );

    // Serialize: wait for previous RPC to finish before writing this one.
    // Timeout starts when the write actually happens, not when sendRPC is called.
    // This prevents queued RPCs from burning timeout while waiting their turn.
    const promise = new Promise<T>((resolve, reject) => {
      const previousQueue = this.writeQueue;
      this.writeQueue = new Promise<void>((releaseQueue) => {
        const releaseAndCleanup = () => releaseQueue();

        previousQueue.then(async () => {
          if (!this._connected) {
            reject(new Error('Not connected'));
            releaseQueue();
            return;
          }

          // NOW start the timeout — the wire is ours
          const sentAt = Date.now();
          this.log('rpc-send', { id, method });

          const timer = setTimeout(() => {
            this.pending.delete(id);
            this.log('rpc-timeout', { id, method, timeoutMs });
            reject(new Error(`RPC timeout: ${method}`));
            releaseQueue();
          }, timeoutMs);

          this.pending.set(id, {
            resolve: (v) => {
              this.log('rpc-ok', { id, method, ms: Date.now() - sentAt });
              clearTimeout(timer);
              resolve(v as T);
              releaseAndCleanup();
            },
            reject: (e) => {
              this.log('rpc-error', { id, method, ms: Date.now() - sentAt, error: e.message });
              clearTimeout(timer);
              reject(e);
              releaseAndCleanup();
            },
            timer,
            method,
          });

          try {
            await this.writer!.write(payload);
          } catch {
            clearTimeout(timer);
            this.pending.delete(id);
            reject(new Error('Not connected'));
            releaseQueue();
          }
        });
      });
    });

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
