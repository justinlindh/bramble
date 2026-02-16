// Web Serial JSON-RPC transport for Bramble devices

export class SerialTransport {
    constructor() {
        this.port = null;
        this.reader = null;
        this.writer = null;
        this.connected = false;
        this._rpcId = 0;
        this._pending = new Map();    // id → { resolve, reject, timer }
        this._notifyCb = null;
        this._readLoop = null;
        this._lineBuf = '';
    }

    async connect(baudRate = 115200) {
        if (!('serial' in navigator)) {
            throw new Error('Web Serial API not supported.');
        }
        this.port = await navigator.serial.requestPort({
            filters: [
                { usbVendorId: 0x303A },
                { usbVendorId: 0x10C4 },
                { usbVendorId: 0x1A86 },
            ]
        });
        await this.port.open({ baudRate });
        this.writer = this.port.writable.getWriter();
        this.reader = this.port.readable.getReader();
        this.connected = true;
        this._lineBuf = '';
        this._startReadLoop();
    }

    _startReadLoop() {
        const decoder = new TextDecoder();
        this._readLoop = (async () => {
            try {
                while (this.connected) {
                    const { value, done } = await this.reader.read();
                    if (done) break;
                    this._lineBuf += decoder.decode(value, { stream: true });
                    this._processLines();
                }
            } catch {
                // port closed or error
            }
        })();
    }

    _processLines() {
        const lines = this._lineBuf.split('\n');
        this._lineBuf = lines.pop(); // keep incomplete line

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
                // Notification from device
                if (this._notifyCb) this._notifyCb(msg.method, msg.params);
            }
        }
    }

    async sendRPC(method, params = {}, timeoutMs = 5000) {
        if (!this.connected) throw new Error('Not connected');
        const id = ++this._rpcId;
        const msg = JSON.stringify({ jsonrpc: '2.0', id, method, params }) + '\n';
        const encoder = new TextEncoder();
        await this.writer.write(encoder.encode(msg));

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

    async disconnect() {
        this.connected = false;
        for (const [id, { reject, timer }] of this._pending) {
            clearTimeout(timer);
            reject(new Error('Disconnected'));
        }
        this._pending.clear();
        try { if (this.reader) { await this.reader.cancel(); this.reader.releaseLock(); } } catch {}
        try { if (this.writer) { this.writer.releaseLock(); } } catch {}
        try { if (this.port) { await this.port.close(); } } catch {}
        this.reader = null;
        this.writer = null;
        this.port = null;
    }
}
