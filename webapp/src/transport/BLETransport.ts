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
// How long the handshake write may spend inside the OS pairing flow before
// we give up. The node's TX characteristic requires an encrypted link, so
// the first write on an unpaired device raises the OS passkey prompt, and
// typing a 6-digit code takes tens of seconds. Aborting earlier (the old 5s
// handshake / 6s write timeouts) killed SMP mid-entry and each failed
// attempt fed the firmware's anti-MITM advertising backoff (1s doubling to
// 60s), which is how a node became invisible to the very next attempt.
const PAIRING_GRACE_MS = 60000;
// Delay between token-write retries while the prompt is up on fail-fast
// stacks (BlueZ rejects the write immediately with a security error rather
// than blocking until pairing completes the way Chrome does).
const PAIRING_RETRY_DELAY_MS = 1000;
// Written during a no-token connect so first-time pairing happens inside
// connect(), under the grace machinery, instead of ambushing the first real
// RPC. Safe on auth-required nodes: ble_rpc_task dispatches pre-auth lines
// starting with '{' to the UNAUTHENTICATED allowlist dispatcher and only
// counts non-JSON first lines as failed token attempts. The reply is never
// awaited: real RPC ids start at 1, so the id 0 response routes nowhere by
// design.
const ENCRYPTION_PROBE_LINE = '{"jsonrpc":"2.0","id":0,"method":"bramble.getVersion","params":{}}';

// True only for the firmware rejecting our RPC token, the one failure that a
// retry cannot fix. It must NOT match the platform's GATT security errors:
// the device requires an encrypted link, so the first write on an unpaired
// link fails with things like "GATT operation not authorized" or
// "insufficient authentication" while the OS runs the pairing prompt, and
// those DO improve on retry. A previous substring match on /auth/ caught them
// too and turned a normal first-pairing into a hard connect failure.
function isTokenRejection(e: unknown): boolean {
  const msg = (e as Error)?.message ?? '';
  return /authentication required/i.test(msg) && !/timed out/i.test(msg);
}

// The platform's transient GATT security errors: what a fail-fast stack
// (BlueZ) returns for a write attempted while the link is not yet encrypted,
// i.e. while the OS pairing prompt is still up. These improve on retry once
// the user finishes typing the passkey. Deliberately disjoint from the two
// terminal shapes: a user cancel (anything containing "cancel") aborts the
// connect, and the firmware's token rejection ("authentication required",
// isTokenRejection above) can never improve on retry.
function isPairingSecurityError(e: unknown): boolean {
  const msg = (e as Error)?.message ?? '';
  if (/cancel/i.test(msg)) return false;
  if (/authentication required/i.test(msg)) return false;
  return /not authorized|insufficient authentication|insufficient encryption/i.test(msg);
}

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
  // Signaled true on the first pairing-security write failure (the OS prompt
  // is up) and false once the handshake write settles, so the UI can show
  // "check your device / OS pairing dialog" instead of a dead spinner.
  private pairingCb: ((pending: boolean) => void) | null = null;
  // Bumped when a GATT session is (re)built or drops. A hung write's timeout
  // closure captures the generation at write time so a stale timer from a
  // dead session can never tear down the healthy session auto-reconnect
  // built in the meantime (latent bug in the old unconditional-disconnect
  // teardown: a pre-drop write's 6s timer outlived the drop and fired after
  // the NEW session was already up).
  private sessionGeneration = 0;

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

    // Chromium caches BluetoothDevice objects per origin, and gatt.disconnect()
    // is fire-and-forget: a quick disconnect/reconnect cycle can hand us a
    // device whose GATT session is half-dead (connect resolves, writes vanish,
    // the handshake times out; only an app restart used to clear it). Reset a
    // stale session first, and retry the whole link once after teardown.
    this.device.addEventListener('gattserverdisconnected', () => {
      this.sessionGeneration++;
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

    if (this.device.gatt?.connected) {
      try { this.device.gatt.disconnect(); } catch { /* best effort */ }
      await new Promise(r => setTimeout(r, 1000));
    }
    // Any final rejection below must tear the link down before it leaves
    // this method: when connect() itself rejects, the store never gets a
    // client to clean up (session.client is only assigned after connect()
    // resolves), and a peripheral with an open GATT link stops advertising,
    // so a leaked link makes the node invisible to every later chooser.
    try {
      // Attempt 1 carries the full pairing grace budget: a first-time pairing
      // (OS passkey prompt and all) must complete inside this attempt, on one
      // link, so the user sees exactly one prompt.
      try {
        await this.establishLink({ graceMs: (this.constructor as typeof BLETransport).pairingGraceMs });
        return;
      } catch (e) {
        const msg = (e as Error)?.message ?? '';
        if (isTokenRejection(e)) {
          throw e; // a real token rejection will not improve on retry
        }
        if (/cancel/i.test(msg)) {
          throw e; // the user dismissed the pairing dialog; a retry re-prompts
        }
        try {
          if (this.device?.gatt?.connected) this.device.gatt.disconnect();
        } catch { /* best effort */ }
        await new Promise(r => setTimeout(r, 1500));
      }
      // One fresh-link retry, with no pairing grace. By the time attempt 1
      // has failed, any OS prompt is dead (NimBLE's SM timeout is about
      // 30s), so this cannot interrupt active passkey typing; it exists to
      // heal Chromium's stale-cached-session wedge described above.
      await this.establishLink({ graceMs: 0 });
    } catch (e) {
      try {
        if (this.device?.gatt?.connected) this.device.gatt.disconnect();
      } catch { /* best effort */ }
      throw e;
    }
  }

  // The (re)connectable part of connect(): everything after device selection.
  // Runs on first connect and on every reconnect attempt against the same
  // BluetoothDevice, including the auth handshake (the firmware requires the
  // token line first on every fresh GATT session). graceMs is the pairing
  // budget for the handshake write: connect() attempt 1 passes the full
  // grace window, everything else passes 0 (a reconnecting session was
  // already paired, and connect() attempt 2 runs after any prompt is dead).
  private async establishLink(opts: { graceMs: number }): Promise<void> {
    if (!this.device) throw new Error('No device selected');
    this.sessionGeneration++;
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
    // Chromium tracks notifications-active per characteristic across GATT
    // sessions. After an unclean previous session, startNotifications can
    // resolve without re-arming event delivery (node logs CCCD enabled and
    // sends the auth reply; the renderer never sees it). Reset the flag
    // first, bounded so a wedged link cannot hang the connect.
    await Promise.race([
      Promise.resolve(this.rxChar.stopNotifications?.()).catch(() => {}),
      new Promise(r => setTimeout(r, 1500)),
    ]);
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
      await this.performAuthHandshake(this.token, opts.graceMs);
    } else {
      // No token still means a write during connect: the encryption probe
      // triggers the link encryption (and, first time, the OS pairing
      // prompt) here, under the grace machinery, instead of leaving it to
      // ambush the first real RPC and its short timeout. See the constant's
      // comment for why this is safe on auth-required firmware.
      await this.writeLineWithPairingGrace(
        new TextEncoder().encode(ENCRYPTION_PROBE_LINE + '\n'),
        opts.graceMs
      );
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
        await this.withAttemptTimeout(this.establishLink({ graceMs: 0 }));
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
  static pairingGraceMs = PAIRING_GRACE_MS;
  static pairingRetryDelayMs = PAIRING_RETRY_DELAY_MS;

  // The store feature-detects this method structurally to surface pairing
  // progress in the connect UI; keep the name stable.
  onPairingStateChange(cb: (pending: boolean) => void): void {
    this.pairingCb = cb;
  }

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
  // The write is awaited FIRST and the 5s reply timer armed only after it
  // succeeds. On a first-time pairing the write IS the pairing flow: Chrome
  // blocks the write promise until the user finishes its pairing dialog, so
  // a reply timer running across the write (the old design) aborted the
  // handshake while the user was still typing the passkey. The write itself
  // is bounded (grace deadline or chunk timeout), so the handshake still
  // cannot hang forever on a half-dead link; and write success implies the
  // link is encrypted and the firmware holds the token line, at which point
  // 5s is plenty for its one-line reply.
  private async performAuthHandshake(token: string, graceMs: number): Promise<void> {
    await this.writeLineWithPairingGrace(new TextEncoder().encode(token + '\n'), graceMs);
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

  // Writes one protocol line, riding out first-time pairing. Fail-fast
  // stacks (BlueZ) reject each write with a transient security error while
  // the OS passkey prompt is up, so those are retried every
  // pairingRetryDelayMs until the grace deadline; Chrome instead blocks the
  // write until its dialog closes, which the per-call chunk timeout (bounded
  // by the time remaining, not the 6s default) rides out. Both terminal
  // outcomes are wrapped into stable contract strings the UI maps:
  // 'Bluetooth pairing was cancelled' and 'Bluetooth pairing did not
  // complete'. Never leaks a raw GATT security string. graceMs 0 means one
  // attempt with the default chunk timeout.
  private async writeLineWithPairingGrace(payload: Uint8Array, graceMs: number): Promise<void> {
    const ctor = this.constructor as typeof BLETransport;
    const deadline = Date.now() + graceMs;
    let pairingSignaled = false;
    try {
      for (;;) {
        const remaining = deadline - Date.now();
        try {
          // Deliberate trade-off: raising the chunk timeout to the grace
          // remainder means the rare Chromium stale-cached-session wedge (a
          // handshake write that hangs on a half-dead session) now waits out
          // the grace before connect()'s fresh-link retry heals it, instead
          // of ~6s. Accepted: a 6s cap kills every Chrome first-time pairing
          // (the blocked write IS the user typing the passkey), while the
          // wedge is rare and still self-heals on attempt 2.
          await this.writeChunked(payload, {
            chunkTimeoutMs: remaining > 0 ? remaining : undefined,
          });
          return;
        } catch (e) {
          const msg = (e as Error)?.message ?? '';
          if (/cancel/i.test(msg)) {
            // The user dismissed the OS pairing dialog. Abort the whole
            // connect: any retry raises a fresh prompt at someone who just
            // said no. The exact wording is a contract with errors.ts
            // (/pairing was cancelled/i).
            throw new Error('Bluetooth pairing was cancelled');
          }
          if (isPairingSecurityError(e)) {
            if (Date.now() < deadline) {
              if (!pairingSignaled) {
                pairingSignaled = true;
                this.pairingCb?.(true);
              }
              await new Promise(r => setTimeout(r, ctor.pairingRetryDelayMs));
              continue;
            }
            // Grace exhausted (or graceMs 0): wrap instead of leaking the
            // platform's GATT string. Contract with errors.ts
            // (/pairing did not complete/i).
            throw new Error('Bluetooth pairing did not complete');
          }
          throw e;
        }
      }
    } finally {
      if (pairingSignaled) this.pairingCb?.(false);
    }
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
  private writeChunked(payload: Uint8Array, opts?: { chunkTimeoutMs?: number }): Promise<void> {
    const run = this.writeQueue.then(async () => {
      if (!this.txChar) throw new Error('Not connected');
      for (let i = 0; i < payload.length; i += BLE_CHUNK_SIZE) {
        await this.writeWithTimeout(payload.slice(i, i + BLE_CHUNK_SIZE), opts?.chunkTimeoutMs);
      }
    });
    // The queue must advance even when a write fails, or one error would
    // wedge every later write behind a rejected promise.
    this.writeQueue = run.catch(() => {});
    return run;
  }

  // A mid-session write that never completes means the link is functionally
  // dead even if GATT still claims connected (field case: outgoing RPCs
  // stopped while notifications kept arriving for 11 minutes). Fail the
  // write AND drop the link so gattserverdisconnected fires and the
  // auto-reconnect loop heals it. The teardown is guarded, not
  // unconditional, for two reasons. First, handshake-phase writes
  // (_connected still false) must NEVER self-disconnect: on Chrome a
  // blocked handshake write IS the user typing the pairing code, and
  // killing the link kills SMP mid-entry (the timeout still rejects, so
  // nothing hangs). Second, the session-generation check: without it a hung
  // write's timer from a dead session could fire after auto-reconnect had
  // already built a NEW session and would disconnect that healthy link.
  private writeWithTimeout(chunk: Uint8Array<ArrayBuffer>, timeoutMsOverride?: number): Promise<void> {
    return new Promise<void>((resolve, reject) => {
      const timeoutMs = timeoutMsOverride
        ?? (this.constructor as typeof BLETransport).writeChunkTimeoutMs;
      const generationAtWrite = this.sessionGeneration;
      const timer = setTimeout(() => {
        reject(new Error('BLE write timed out'));
        if (generationAtWrite !== this.sessionGeneration || !this._connected) return;
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
    // Fire-and-forget (NOT awaited: a GATT round-trip can hang on a wedged
    // link, and awaiting it here once leaked a live connection for 11
    // minutes). The call still clears Chromium's notifications-active flag
    // so the next session re-arms event delivery.
    try { Promise.resolve(this.rxChar?.stopNotifications?.()).catch(() => {}); } catch { /* ignore */ }
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
