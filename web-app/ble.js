// Web Bluetooth JSON-RPC transport for Bramble devices

const SERVICE_UUID = '6e400001-b5a3-f393-e0a9-e50e24dcca9e';
const TX_CHAR_UUID = '6e400002-b5a3-f393-e0a9-e50e24dcca9e'; // write to device
const RX_CHAR_UUID = '6e400003-b5a3-f393-e0a9-e50e24dcca9e'; // notifications from device

export class BLETransport {
    constructor() {
        this.device = null;
        this.txChar = null;
        this.rxChar = null;
        this.connected = false;
        this._rpcId = 0;
        this._pending = new Map();
        this._notifyCb = null;
        this._lineBuf = '';
    }

    async connect() {
        if (!('bluetooth' in navigator)) {
            throw new Error('Web Bluetooth API not supported.');
        }
        this.device = await navigator.bluetooth.requestDevice({
            filters: [{ services: [SERVICE_UUID] }],
            optionalServices: [SERVICE_UUID],
        });

        this.device.addEventListener('gattserverdisconnected', () => {
            this.connected = false;
            this._rejectAll('BLE disconnected');
        });

        const server = await this.device.gatt.connect();
        const service = await server.getPrimaryService(SERVICE_UUID);
        this.txChar = await service.getCharacteristic(TX_CHAR_UUID);
        this.rxChar = await service.getCharacteristic(RX_CHAR_UUID);

        await this.rxChar.startNotifications();
        this.rxChar.addEventListener('characteristicvaluechanged', (e) => {
            const decoder = new TextDecoder();
            this._lineBuf += decoder.decode(e.target.value, { stream: true });
            this._processLines();
        });

        this.connected = true;
    }

    _processLines() {
        const lines = this._lineBuf.split('\n');
        this._lineBuf = lines.pop();

        for (const line of lines) {
            const trimmed = line.trim();
            if (!trimmed) continue;
            let msg;
            try { msg = JSON.parse(trimmed); } catch { continue; }

            if ('id' in msg && this._pending.has(msg.id)) {
                const { resolve, reject, timer } = this._pending.get(msg.id);
                clearTimeout(timer);
                this._pending.delete(msg.id);
                if (msg.error) reject(new Error(msg.error.message || JSON.stringify(msg.error)));
                else resolve(msg.result);
            } else if (msg.method && !('id' in msg)) {
                if (this._notifyCb) this._notifyCb(msg.method, msg.params);
            }
        }
    }

    async sendRPC(method, params = {}, timeoutMs = 5000) {
        if (!this.connected) throw new Error('Not connected');
        const id = ++this._rpcId;
        const msg = JSON.stringify({ jsonrpc: '2.0', id, method, params }) + '\n';
        const encoder = new TextEncoder();
        const data = encoder.encode(msg);

        // BLE has a 20-byte MTU typically, chunk writes
        const chunkSize = 20;
        for (let i = 0; i < data.length; i += chunkSize) {
            await this.txChar.writeValueWithResponse(data.slice(i, i + chunkSize));
        }

        return new Promise((resolve, reject) => {
            const timer = setTimeout(() => {
                this._pending.delete(id);
                reject(new Error(`RPC timeout: ${method}`));
            }, timeoutMs);
            this._pending.set(id, { resolve, reject, timer });
        });
    }

    onNotification(callback) {
        this._notifyCb = callback;
    }

    _rejectAll(reason) {
        for (const [id, { reject, timer }] of this._pending) {
            clearTimeout(timer);
            reject(new Error(reason));
        }
        this._pending.clear();
    }

    async disconnect() {
        this.connected = false;
        this._rejectAll('Disconnected');
        try { if (this.device?.gatt?.connected) this.device.gatt.disconnect(); } catch {}
        this.device = null;
        this.txChar = null;
        this.rxChar = null;
    }
}
