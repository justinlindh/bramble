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

export class BLETransport implements Transport {
  private device: BluetoothDevice | null = null;
  private txChar: BluetoothRemoteGATTCharacteristic | null = null;
  private rxChar: BluetoothRemoteGATTCharacteristic | null = null;
  private _connected = false;
  private rpcId = 0;
  private pending = new Map<number, Pending>();
  private notifyCb: ((method: string, params: unknown) => void) | null = null;
  private lineBuf = '';

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
    this._connected = true;
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
    if (!this._connected || !this.txChar) throw new Error('Not connected');
    const id = ++this.rpcId;
    const payload = new TextEncoder().encode(
      JSON.stringify({ jsonrpc: '2.0', id, method, params }) + '\n'
    );

    // BLE MTU ≈ 20 bytes on most implementations; chunk writes
    for (let i = 0; i < payload.length; i += BLE_CHUNK_SIZE) {
      await this.txChar.writeValueWithResponse(payload.slice(i, i + BLE_CHUNK_SIZE));
    }

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
