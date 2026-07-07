import type { Transport } from '../types/bramble';

const NUS_SERVICE = '6e400001-b5a3-f393-e0a9-e50e24dcca9e';
const NUS_TX = '6e400002-b5a3-f393-e0a9-e50e24dcca9e'; // write (app → device)
const NUS_RX = '6e400003-b5a3-f393-e0a9-e50e24dcca9e'; // notify (device → app)
const BLE_CHUNK_SIZE = 20;

interface Pending {
  resolve: (v: unknown) => void;
  reject: (e: Error) => void;
  timer: ReturnType<typeof setTimeout>;
}

interface AuthWaiter {
  resolve: () => void;
  reject: (e: Error) => void;
}

const AUTH_HANDSHAKE_TIMEOUT_MS = 5000;

export class BLETransport implements Transport {
  private device: BluetoothDevice | null = null;
  private txChar: BluetoothRemoteGATTCharacteristic | null = null;
  private rxChar: BluetoothRemoteGATTCharacteristic | null = null;
  private _connected = false;
  private rpcId = 0;
  private pending = new Map<number, Pending>();
  private notifyCb: ((method: string, params: unknown) => void) | null = null;
  private lineBuf = '';
  private readonly token?: string;
  // Set only while the one-shot auth handshake line is outstanding. The very
  // next parsed RX line is treated as the handshake result and consumed here
  // instead of the normal id/notification routing below, then cleared so
  // routing resumes as usual.
  private pendingAuth: AuthWaiter | null = null;

  constructor(token?: string) {
    this.token = token;
  }

  get connected() { return this._connected; }

  async connect(): Promise<void> {
    if (!('bluetooth' in navigator)) throw new Error('Web Bluetooth not supported');

    const bluetooth = (navigator as { bluetooth: Bluetooth }).bluetooth;
    this.device = await bluetooth.requestDevice({
      filters: [{ services: [NUS_SERVICE] }],
      optionalServices: [NUS_SERVICE],
    });

    this.device.addEventListener('gattserverdisconnected', () => {
      this._connected = false;
      this.rejectAll(new Error('BLE disconnected'));
    });

    const server = await this.device.gatt!.connect();
    const service = await server.getPrimaryService(NUS_SERVICE);
    this.txChar = await service.getCharacteristic(NUS_TX);
    this.rxChar = await service.getCharacteristic(NUS_RX);
    await this.rxChar.startNotifications();
    this.rxChar.addEventListener(
      'characteristicvaluechanged',
      this.onBLEData.bind(this) as EventListener
    );

    // Fresh connections with auth enabled require the bare token (not JSON-RPC)
    // as the first TX write, before any sendRPC call is allowed to run.
    if (this.token) {
      await this.performAuthHandshake(this.token);
    }

    this._connected = true;
  }

  // Writes the token line directly, bypassing sendRPC (it carries no id and
  // is not JSON-RPC), then waits for the single RX line the firmware answers
  // with. Rejects with a message the existing auth-error UI matches
  // (/1008|unauthorized|auth/i), mirroring the WiFi path's wording.
  private async performAuthHandshake(token: string): Promise<void> {
    await this.writeChunked(new TextEncoder().encode(token + '\n'));

    return new Promise<void>((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pendingAuth = null;
        reject(new Error('Authentication handshake timed out'));
      }, AUTH_HANDSHAKE_TIMEOUT_MS);
      this.pendingAuth = {
        resolve: () => { clearTimeout(timer); resolve(); },
        reject: (err: Error) => { clearTimeout(timer); reject(err); },
      };
    });
  }

  private onBLEData(e: Event): void {
    const target = e.target as BluetoothRemoteGATTCharacteristic;
    this.lineBuf += new TextDecoder().decode(target.value!, { stream: true });
    this.processLines();
  }

  private processLines(): void {
    const lines = this.lineBuf.split('\n');
    this.lineBuf = lines.pop() ?? '';
    for (const raw of lines) {
      const line = raw.trim();
      if (!line) continue;
      let msg: Record<string, unknown>;
      try { msg = JSON.parse(line); } catch { continue; }

      if (this.pendingAuth) {
        const waiter = this.pendingAuth;
        this.pendingAuth = null;
        const result = msg.result as { ok?: boolean } | undefined;
        if (result?.ok === true) {
          waiter.resolve();
        } else {
          const errMsg = (msg.error as { message?: string } | undefined)?.message ?? 'unauthorized';
          waiter.reject(new Error(`Authentication required: ${errMsg}`));
        }
        continue;
      }

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

  // Serialize whole-line writes: the firmware reassembles requests from a
  // byte stream, so two concurrent sendRPC calls (a user send racing the 10s
  // background polls, say) interleaving their 20-byte chunks corrupt BOTH
  // lines and both RPCs time out. Same hazard SerialTransport guards with
  // its writeQueue.
  private writeQueue: Promise<void> = Promise.resolve();

  // BLE MTU ~= 20 bytes on most implementations; chunk writes. Shared by
  // sendRPC and the auth handshake, which writes a bare token line instead
  // of a JSON-RPC payload. One payload's chunks are written atomically with
  // respect to other writeChunked callers.
  private writeChunked(payload: Uint8Array): Promise<void> {
    const run = this.writeQueue.then(async () => {
      if (!this.txChar) throw new Error('Not connected');
      for (let i = 0; i < payload.length; i += BLE_CHUNK_SIZE) {
        await this.txChar.writeValueWithResponse(payload.slice(i, i + BLE_CHUNK_SIZE));
      }
    });
    // The queue must advance even when a write fails, or one error would
    // wedge every later write behind a rejected promise.
    this.writeQueue = run.catch(() => {});
    return run;
  }

  async sendRPC<T>(method: string, params: Record<string, unknown> = {}, timeoutMs = 5000): Promise<T> {
    if (!this._connected || !this.txChar) throw new Error('Not connected');
    const id = ++this.rpcId;
    const payload = new TextEncoder().encode(
      JSON.stringify({ jsonrpc: '2.0', id, method, params }) + '\n'
    );
    await this.writeChunked(payload);

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
    if (this.pendingAuth) {
      const waiter = this.pendingAuth;
      this.pendingAuth = null;
      waiter.reject(err);
    }
  }

  async disconnect(): Promise<void> {
    this._connected = false;
    this.rejectAll(new Error('Disconnected'));
    try {
      if (this.rxChar) {
        await this.rxChar.stopNotifications();
      }
    } catch { /* ignore */ }
    try {
      if (this.device?.gatt?.connected) {
        this.device.gatt.disconnect();
      }
    } catch { /* ignore */ }
    this.txChar = null;
    this.rxChar = null;
    this.device = null;
  }
}
