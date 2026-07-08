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

interface BleReconnectCallbacks {
  onDisconnect?: () => void;
  onReconnect?: () => void;
}

const AUTH_HANDSHAKE_TIMEOUT_MS = 5000;
const RECONNECT_INITIAL_DELAY_MS = 2000;
const RECONNECT_MAX_DELAY_MS = 30000;
// Bounds one whole reconnect attempt (GATT connect + discovery +
// notifications + auth). Android's BLE stack can simply never deliver the
// connect callback after a peer died mid-connection, and an unbounded await
// there killed the retry loop silently in the field. Static so tests can
// shrink it.
const ESTABLISH_LINK_TIMEOUT_MS = 20000;
// Bounds a single GATT chunk write. Desktop BlueZ occasionally never
// resolves writeValueWithResponse; the serialized write queue then wedges
// silently (outgoing RPCs stop, incoming notifications keep flowing).
const WRITE_CHUNK_TIMEOUT_MS = 6000;

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

  // Auto-reconnect: mirrors WebSocketTransport so the store's duck-typed
  // enableAutoReconnect wiring (banner + full state refetch) works unchanged.
  // The device handle survives a GATT drop, so re-establishing the link needs
  // no new device picker: reconnect re-runs gatt.connect + service discovery +
  // the auth handshake against the SAME BluetoothDevice.
  private autoReconnect = false;
  private reconnectCbs: BleReconnectCallbacks = {};
  private reconnectTimer: ReturnType<typeof setTimeout> | null = null;
  private reconnectDelay = RECONNECT_INITIAL_DELAY_MS;
  private intentionalClose = false;

  constructor(token?: string, device?: BluetoothDevice) {
    this.token = token;
    // A device chosen up front (pick-first flow / saved-device reconnect)
    // skips the chooser inside connect().
    this.device = device ?? null;
  }

  /**
   * Runs the device chooser and returns the picked device. Separated from
   * connect() so the UI can pick first and connect later. `expected` is the
   * saved-device fast path: on Android the polyfill connects straight to the
   * stored MAC (no chooser); on desktop the Electron main process auto-picks
   * the matching candidate (renderer arms it via brambleDesktop).
   */
  static async pickDevice(expected?: { id?: string; name?: string }): Promise<BluetoothDevice> {
    if (!('bluetooth' in navigator)) throw new Error('Web Bluetooth not supported');
    const bluetooth = (navigator as { bluetooth: Bluetooth }).bluetooth;
    if (expected && window.brambleDesktop?.autoSelectNextDevice) {
      window.brambleDesktop.autoSelectNextDevice(expected);
    }
    try {
      const options: RequestDeviceOptions & Record<string, unknown> = {
        filters: [{ services: [NUS_SERVICE] }],
        optionalServices: [NUS_SERVICE],
      };
      if (expected?.id) {
        // Nonstandard passthrough consumed by the Android polyfill only.
        options.brambleExpectedDeviceId = expected.id;
        if (expected.name) options.brambleExpectedName = expected.name;
      }
      return await bluetooth.requestDevice(options as RequestDeviceOptions);
    } finally {
      window.brambleDesktop?.autoSelectNextDevice?.(null);
    }
  }

  get connected() { return this._connected; }

  enableAutoReconnect(cbs: BleReconnectCallbacks): void {
    this.autoReconnect = true;
    this.reconnectCbs = cbs;
    if (typeof document !== 'undefined') {
      document.addEventListener('visibilitychange', this.onVisibilityKick);
    }
  }

  async connect(): Promise<void> {
    if (!this.device) {
      this.device = await BLETransport.pickDevice();
    }
    this.intentionalClose = false;

    this.device.addEventListener('gattserverdisconnected', () => {
      this._connected = false;
      this.rejectAll(new Error('BLE disconnected'));
      // Walking out of range fires this within seconds (supervision
      // timeout). Heal instead of dying silently: tell the UI and start
      // retrying so walking back into range reconnects by itself.
      if (this.autoReconnect && !this.intentionalClose) {
        this.reconnectCbs.onDisconnect?.();
        this.scheduleReconnect();
      }
    });

    await this.establishLink();
  }

  // The (re)connectable part of connect(): everything after device selection.
  // Runs on first connect and on every reconnect attempt against the same
  // BluetoothDevice, including the auth handshake (the firmware requires the
  // token line first on every fresh GATT session).
  private async establishLink(): Promise<void> {
    if (!this.device) throw new Error('No device selected');
    this.lineBuf = '';
    // A write that was in flight when the link died may never settle (the
    // Android bridge cannot deliver its completion callback for a dead GATT
    // session). The queue chains on that promise, so without a reset every
    // write on the NEW session - starting with the auth token - would queue
    // behind it forever and the reconnect could never succeed.
    this.writeQueue = Promise.resolve();

    const server = await this.device.gatt!.connect();
    const service = await server.getPrimaryService(NUS_SERVICE);
    this.txChar = await service.getCharacteristic(NUS_TX);
    this.rxChar = await service.getCharacteristic(NUS_RX);
    await this.rxChar.startNotifications();
    // The Android polyfill hands back the SAME characteristic object across
    // reconnects, so a plain addEventListener per attempt would stack
    // duplicate listeners and double-fire every notification. Remove first
    // (no-op when the object is fresh, as in real Web Bluetooth).
    this.rxChar.removeEventListener('characteristicvaluechanged', this.boundOnBLEData);
    this.rxChar.addEventListener('characteristicvaluechanged', this.boundOnBLEData);

    // Fresh connections with auth enabled require the bare token (not JSON-RPC)
    // as the first TX write, before any sendRPC call is allowed to run.
    if (this.token) {
      await this.performAuthHandshake(this.token);
    }

    this._connected = true;
  }

  private scheduleReconnect(): void {
    if (!this.autoReconnect || this.intentionalClose || this.reconnectTimer) return;
    const delay = Math.min(this.reconnectDelay, RECONNECT_MAX_DELAY_MS);
    this.reconnectTimer = setTimeout(async () => {
      this.reconnectTimer = null;
      if (this.intentionalClose) return;
      try {
        await this.withAttemptTimeout(this.establishLink());
        this.reconnectDelay = RECONNECT_INITIAL_DELAY_MS;
        this.reconnectCbs.onReconnect?.();
      } catch {
        // A failed attempt can leave a half-open GATT connection standing
        // (connected at the radio, auth never completed). Tear it down before
        // retrying: a BLE peripheral stops advertising while a connection is
        // open, so a zombie session blocks the node for everyone - including
        // our own next attempt.
        try {
          if (this.device?.gatt?.connected) this.device.gatt.disconnect();
        } catch { /* best effort */ }
        this.reconnectDelay = Math.min(this.reconnectDelay * 1.5, RECONNECT_MAX_DELAY_MS);
        this.scheduleReconnect();
      }
    }, delay);
  }

  // The retry loop must be unkillable: every attempt settles, by result or
  // by this timeout. Note the underlying native call is not cancelled; a
  // late success is handled by the next attempt finding gatt.connected and
  // the teardown in the catch path.
  private withAttemptTimeout<T>(p: Promise<T>): Promise<T> {
    const timeoutMs = (this.constructor as typeof BLETransport).establishLinkTimeoutMs;
    return new Promise<T>((resolve, reject) => {
      const timer = setTimeout(() => reject(new Error('BLE reconnect attempt timed out')), timeoutMs);
      p.then(
        (v) => { clearTimeout(timer); resolve(v); },
        (e) => { clearTimeout(timer); reject(e); }
      );
    });
  }

  static establishLinkTimeoutMs = ESTABLISH_LINK_TIMEOUT_MS;
  static writeChunkTimeoutMs = WRITE_CHUNK_TIMEOUT_MS;

  // Background timers are throttled aggressively on Android, so a 30s-capped
  // backoff can stretch much longer while the screen is off. When the user
  // opens the app, retry immediately instead of waiting out the current
  // backoff.
  private onVisibilityKick = () => {
    if (typeof document === 'undefined' || document.visibilityState !== 'visible') return;
    if (!this.autoReconnect || this.intentionalClose || this._connected) return;
    if (this.reconnectTimer) {
      clearTimeout(this.reconnectTimer);
      this.reconnectTimer = null;
    }
    this.reconnectDelay = RECONNECT_INITIAL_DELAY_MS;
    this.scheduleReconnect();
  };

  // Writes the token line directly, bypassing sendRPC (it carries no id and
  // is not JSON-RPC), then waits for the single RX line the firmware answers
  // with. Rejects with a message the existing auth-error UI matches
  // (/1008|unauthorized|auth/i), mirroring the WiFi path's wording.
  //
  // The timeout is armed BEFORE the token write, bounding the whole
  // handshake: on a half-dead link the GATT write itself can hang forever,
  // and awaiting it first would freeze connect/reconnect with no timeout
  // ever starting.
  private performAuthHandshake(token: string): Promise<void> {
    return new Promise<void>((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pendingAuth = null;
        reject(new Error('Authentication handshake timed out'));
      }, AUTH_HANDSHAKE_TIMEOUT_MS);
      this.pendingAuth = {
        resolve: () => { clearTimeout(timer); resolve(); },
        reject: (err: Error) => { clearTimeout(timer); reject(err); },
      };
      this.writeChunked(new TextEncoder().encode(token + '\n')).catch((e) => {
        // Failed write: fail the handshake now instead of waiting out the timer.
        if (this.pendingAuth) {
          const waiter = this.pendingAuth;
          this.pendingAuth = null;
          waiter.reject(e as Error);
        }
      });
    });
  }

  private boundOnBLEData = this.onBLEData.bind(this) as EventListener;

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
        await this.writeWithTimeout(payload.slice(i, i + BLE_CHUNK_SIZE));
      }
    });
    // The queue must advance even when a write fails, or one error would
    // wedge every later write behind a rejected promise.
    this.writeQueue = run.catch(() => {});
    return run;
  }

  // A write that never completes means the link is functionally dead even if
  // GATT still claims connected (field case: outgoing RPCs stopped while
  // notifications kept arriving for 11 minutes). Fail the write AND drop the
  // link so gattserverdisconnected fires and the auto-reconnect loop heals it.
  private writeWithTimeout(chunk: Uint8Array<ArrayBuffer>): Promise<void> {
    return new Promise<void>((resolve, reject) => {
      const timeoutMs = (this.constructor as typeof BLETransport).writeChunkTimeoutMs;
      const timer = setTimeout(() => {
        reject(new Error('BLE write timed out'));
        try {
          if (this.device?.gatt?.connected) this.device.gatt.disconnect();
        } catch { /* best effort */ }
      }, timeoutMs);
      this.txChar!.writeValueWithResponse(chunk).then(
        () => { clearTimeout(timer); resolve(); },
        (e) => { clearTimeout(timer); reject(e as Error); }
      );
    });
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
    this.intentionalClose = true;
    if (typeof document !== 'undefined') {
      document.removeEventListener('visibilitychange', this.onVisibilityKick);
    }
    if (this.reconnectTimer) {
      clearTimeout(this.reconnectTimer);
      this.reconnectTimer = null;
    }
    this._connected = false;
    this.rejectAll(new Error('Disconnected'));
    // No stopNotifications here: it is a GATT round-trip that can hang on a
    // wedged link, and awaiting it before gatt.disconnect() leaked a live
    // connection for 11 minutes in the field (the node stops advertising the
    // whole time). Dropping the GATT link implicitly ends the subscription.
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
