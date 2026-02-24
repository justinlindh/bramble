// Bramble Web Flasher — ESP32 bootloader protocol over Web Serial
// Based on esptool serial protocol (SLIP framing + ROM bootloader commands)

class BrambleFlasher {
    constructor() {
        this.port = null;
        this.reader = null;
        this.writer = null;
        this.connected = false;
        this._readBuffer = new Uint8Array(0);
    }

    // SLIP framing constants
    static SLIP_END     = 0xC0;
    static SLIP_ESC     = 0xDB;
    static SLIP_ESC_END = 0xDC;
    static SLIP_ESC_ESC = 0xDD;

    // ESP32 ROM bootloader commands
    static CMD_FLASH_BEGIN    = 0x02;
    static CMD_FLASH_DATA     = 0x03;
    static CMD_FLASH_END      = 0x04;
    static CMD_MEM_BEGIN      = 0x05;
    static CMD_MEM_END        = 0x06;
    static CMD_MEM_DATA       = 0x07;
    static CMD_SYNC           = 0x08;
    static CMD_READ_REG       = 0x0A;
    static CMD_CHANGE_BAUDRATE = 0x0F;
    static CMD_SPI_SET_PARAMS = 0x0B;
    static CMD_SPI_ATTACH     = 0x0D;
    static CMD_SPI_FLASH_MD5  = 0x13;

    static FLASH_BLOCK_SIZE = 0x400; // 1KB per data packet
    static SYNC_TIMEOUT_MS  = 3000;
    static RESPONSE_TIMEOUT_MS = 5000;
    static MAX_SYNC_ATTEMPTS = 5;

    // Board configurations
    static BOARDS = {
        'heltec-v3': {
            name: 'Heltec WiFi LoRa 32 V3',
            chipType: 'ESP32-S3',
            flashSize: '8MB',
            partitions: [
                { name: 'bootloader',      offset: 0x0000,  file: 'bootloader.bin' },
                { name: 'partition-table',  offset: 0x8000,  file: 'partition-table.bin' },
                { name: 'firmware',         offset: 0x10000, file: 'bramble.bin' }
            ]
        },
        'tdeck-plus': {
            name: 'LILYGO T-Deck Plus',
            chipType: 'ESP32-S3',
            flashSize: '16MB',
            partitions: [
                { name: 'bootloader',      offset: 0x0000,  file: 'bootloader.bin' },
                { name: 'partition-table',  offset: 0x8000,  file: 'partition-table.bin' },
                { name: 'firmware',         offset: 0x10000, file: 'bramble.bin' }
            ]
        },
        'heltec-v4': {
            name: 'Heltec V4',
            chipType: 'ESP32-S3',
            flashSize: '8MB',
            partitions: [
                { name: 'bootloader',      offset: 0x0000,  file: 'bootloader.bin' },
                { name: 'partition-table',  offset: 0x8000,  file: 'partition-table.bin' },
                { name: 'firmware',         offset: 0x10000, file: 'bramble.bin' }
            ]
        }
    };

    // ── Connection ──────────────────────────────────────────────

    async connect() {
        if (!('serial' in navigator)) {
            throw new Error('Web Serial API not supported. Use Chrome or Edge.');
        }
        this.port = await navigator.serial.requestPort({
            filters: [
                { usbVendorId: 0x303A }, // Espressif
                { usbVendorId: 0x10C4 }, // CP2102
                { usbVendorId: 0x1A86 }, // CH340
            ]
        });
        await this.port.open({ baudRate: 115200 });
        this.writer = this.port.writable.getWriter();
        this.reader = this.port.readable.getReader();
        this.connected = true;
        this._readBuffer = new Uint8Array(0);
    }

    async disconnect() {
        this.connected = false;
        try { if (this.reader) { await this.reader.cancel(); this.reader.releaseLock(); } } catch {}
        try { if (this.writer) { this.writer.releaseLock(); } } catch {}
        try { if (this.port) { await this.port.close(); } } catch {}
        this.reader = null;
        this.writer = null;
        this.port = null;
    }

    // ── SLIP Framing ────────────────────────────────────────────

    slipEncode(data) {
        const out = [BrambleFlasher.SLIP_END];
        for (const b of data) {
            if (b === BrambleFlasher.SLIP_END) {
                out.push(BrambleFlasher.SLIP_ESC, BrambleFlasher.SLIP_ESC_END);
            } else if (b === BrambleFlasher.SLIP_ESC) {
                out.push(BrambleFlasher.SLIP_ESC, BrambleFlasher.SLIP_ESC_ESC);
            } else {
                out.push(b);
            }
        }
        out.push(BrambleFlasher.SLIP_END);
        return new Uint8Array(out);
    }

    slipDecode(data) {
        const out = [];
        let esc = false;
        for (const b of data) {
            if (esc) {
                if (b === BrambleFlasher.SLIP_ESC_END) out.push(BrambleFlasher.SLIP_END);
                else if (b === BrambleFlasher.SLIP_ESC_ESC) out.push(BrambleFlasher.SLIP_ESC);
                else out.push(b);
                esc = false;
            } else if (b === BrambleFlasher.SLIP_ESC) {
                esc = true;
            } else if (b !== BrambleFlasher.SLIP_END) {
                out.push(b);
            }
        }
        return new Uint8Array(out);
    }

    // ── Low-level I/O ───────────────────────────────────────────

    async writeRaw(data) {
        await this.writer.write(data);
    }

    async readRaw(timeoutMs = BrambleFlasher.RESPONSE_TIMEOUT_MS) {
        const deadline = Date.now() + timeoutMs;
        while (Date.now() < deadline) {
            const remaining = deadline - Date.now();
            const result = await Promise.race([
                this.reader.read(),
                new Promise((_, rej) => setTimeout(() => rej(new Error('Read timeout')), remaining))
            ]);
            if (result.done) throw new Error('Serial port closed');
            return result.value;
        }
        throw new Error('Read timeout');
    }

    async readSlipPacket(timeoutMs = BrambleFlasher.RESPONSE_TIMEOUT_MS) {
        const deadline = Date.now() + timeoutMs;
        const buf = [];
        let inPacket = false;

        while (Date.now() < deadline) {
            let chunk;
            try {
                chunk = await this.readRaw(deadline - Date.now());
            } catch {
                break;
            }

            for (const b of chunk) {
                if (b === BrambleFlasher.SLIP_END) {
                    if (inPacket && buf.length > 0) {
                        return this.slipDecode(new Uint8Array(buf));
                    }
                    inPacket = true;
                    buf.length = 0;
                } else if (inPacket) {
                    buf.push(b);
                }
            }
        }
        throw new Error('Timeout waiting for SLIP packet');
    }

    // ── Command Building ────────────────────────────────────────

    buildCommand(opcode, data, checksum = 0) {
        const dataLen = data.length;
        // Header: direction(1) + command(1) + size(2) + checksum(4)
        const pkt = new Uint8Array(8 + dataLen);
        const view = new DataView(pkt.buffer);
        view.setUint8(0, 0x00);              // direction: request
        view.setUint8(1, opcode);             // command
        view.setUint16(2, dataLen, true);     // data length (LE)
        view.setUint32(4, checksum, true);    // checksum (LE)
        pkt.set(data, 8);
        return pkt;
    }

    parseResponse(data) {
        if (data.length < 8) return null;
        const view = new DataView(data.buffer, data.byteOffset, data.byteLength);
        return {
            direction: view.getUint8(0),  // 1 = response
            command:   view.getUint8(1),
            size:      view.getUint16(2, true),
            value:     view.getUint32(4, true),
            data:      data.slice(8),
            status:    data.length > 8 ? data[8] : 0,
            error:     data.length > 9 ? data[9] : 0,
        };
    }

    checksum(data) {
        let cs = 0xEF;
        for (const b of data) cs ^= b;
        return cs;
    }

    async sendCommand(opcode, data = new Uint8Array(0), checksum = 0, timeoutMs) {
        const pkt = this.buildCommand(opcode, data, checksum);
        await this.writeRaw(this.slipEncode(pkt));
        const resp = await this.readSlipPacket(timeoutMs);
        return this.parseResponse(resp);
    }

    // ── Sync ────────────────────────────────────────────────────

    async sync() {
        // Sync payload: 0x07 0x07 0x12 0x20 + 32 × 0x55
        const syncData = new Uint8Array(36);
        syncData[0] = 0x07; syncData[1] = 0x07;
        syncData[2] = 0x12; syncData[3] = 0x20;
        for (let i = 4; i < 36; i++) syncData[i] = 0x55;

        for (let attempt = 0; attempt < BrambleFlasher.MAX_SYNC_ATTEMPTS; attempt++) {
            try {
                // Toggle DTR/RTS to enter bootloader
                await this.port.setSignals({ dataTerminalReady: false, requestToSend: true });
                await sleep(100);
                await this.port.setSignals({ dataTerminalReady: true, requestToSend: false });
                await sleep(50);
                await this.port.setSignals({ dataTerminalReady: false });

                // Flush any pending data
                await sleep(200);

                const resp = await this.sendCommand(
                    BrambleFlasher.CMD_SYNC, syncData, 0,
                    BrambleFlasher.SYNC_TIMEOUT_MS
                );
                if (resp && resp.direction === 1) {
                    return true;
                }
            } catch {
                // Retry
            }
        }
        throw new Error('Failed to sync with bootloader. Hold BOOT button and try again.');
    }

    // ── Flash Operations ────────────────────────────────────────

    async spiAttach() {
        const data = new Uint8Array(8); // all zeros = default SPI config
        await this.sendCommand(BrambleFlasher.CMD_SPI_ATTACH, data);
    }

    async flashBegin(size, offset, blockSize = BrambleFlasher.FLASH_BLOCK_SIZE) {
        const numBlocks = Math.ceil(size / blockSize);
        const eraseSize = numBlocks * blockSize;

        const data = new Uint8Array(16);
        const view = new DataView(data.buffer);
        view.setUint32(0, eraseSize, true);   // erase size
        view.setUint32(4, numBlocks, true);    // number of blocks
        view.setUint32(8, blockSize, true);    // block size
        view.setUint32(12, offset, true);      // flash offset

        const resp = await this.sendCommand(BrambleFlasher.CMD_FLASH_BEGIN, data);
        if (resp && resp.status !== 0) {
            throw new Error(`flash_begin failed: status=${resp.status} error=${resp.error}`);
        }
    }

    async flashBlock(data, seq) {
        const blockSize = BrambleFlasher.FLASH_BLOCK_SIZE;
        // Pad to block size
        let padded = data;
        if (data.length < blockSize) {
            padded = new Uint8Array(blockSize);
            padded.set(data);
            padded.fill(0xFF, data.length);
        }

        const header = new Uint8Array(16);
        const view = new DataView(header.buffer);
        view.setUint32(0, padded.length, true);  // data size
        view.setUint32(4, seq, true);             // sequence number
        view.setUint32(8, 0, true);               // reserved
        view.setUint32(12, 0, true);              // reserved

        const pkt = new Uint8Array(header.length + padded.length);
        pkt.set(header);
        pkt.set(padded, header.length);

        const cs = this.checksum(padded);
        const resp = await this.sendCommand(BrambleFlasher.CMD_FLASH_DATA, pkt, cs);
        if (resp && resp.status !== 0) {
            throw new Error(`flash_data failed at block ${seq}: status=${resp.status}`);
        }
    }

    async flashEnd(reboot = false) {
        const data = new Uint8Array(4);
        new DataView(data.buffer).setUint32(0, reboot ? 0 : 1, true);
        await this.sendCommand(BrambleFlasher.CMD_FLASH_END, data);
    }

    async flashRegion(address, binData, onProgress) {
        const blockSize = BrambleFlasher.FLASH_BLOCK_SIZE;
        const numBlocks = Math.ceil(binData.length / blockSize);

        await this.flashBegin(binData.length, address, blockSize);

        for (let i = 0; i < numBlocks; i++) {
            const start = i * blockSize;
            const end = Math.min(start + blockSize, binData.length);
            const block = binData.slice(start, end);
            await this.flashBlock(block, i);
            if (onProgress) onProgress(i + 1, numBlocks);
        }

        await this.flashEnd(false);
    }

    async flashMD5(address, size) {
        const data = new Uint8Array(16);
        const view = new DataView(data.buffer);
        view.setUint32(0, address, true);
        view.setUint32(4, size, true);
        view.setUint32(8, 0, true);
        view.setUint32(12, 0, true);
        const resp = await this.sendCommand(BrambleFlasher.CMD_SPI_FLASH_MD5, data, 0, 10000);
        if (resp && resp.data.length >= 16) {
            return Array.from(resp.data.slice(0, 16))
                .map(b => b.toString(16).padStart(2, '0')).join('');
        }
        return null;
    }

    // ── High-level Flash ────────────────────────────────────────

    async flashFirmware(boardType, releaseCtx, onLog, onProgress) {
        const board = BrambleFlasher.BOARDS[boardType];
        if (!board) throw new Error(`Unknown board: ${boardType}`);

        onLog(`Board: ${board.name} (${board.chipType}, ${board.flashSize})`);
        onLog('Syncing with bootloader...');
        await this.sync();
        onLog('✓ Synced');

        onLog('Attaching SPI flash...');
        await this.spiAttach();
        onLog('✓ SPI attached');

        const totalPartitions = board.partitions.length;
        let partIdx = 0;

        for (const part of board.partitions) {
            partIdx++;
            let url = `firmware/${boardType}/${part.file}`;
            if (releaseCtx && releaseCtx.artifactsByFile && releaseCtx.artifactsByFile[part.file]) {
                url = releaseCtx.artifactsByFile[part.file].file;
            }
            onLog(`\nFetching ${part.name} (${part.file})...`);

            let binData;
            try {
                const resp = await fetch(url);
                if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
                binData = new Uint8Array(await resp.arrayBuffer());
            } catch (e) {
                throw new Error(`Failed to fetch firmware artifact (${part.file}): ${e.message}`);
            }

            onLog(`  Size: ${binData.length} bytes → 0x${part.offset.toString(16)}`);
            onLog(`  Flashing ${part.name}...`);

            await this.flashRegion(part.offset, binData, (block, total) => {
                const partProgress = (partIdx - 1) / totalPartitions;
                const blockProgress = block / total / totalPartitions;
                onProgress(Math.round((partProgress + blockProgress) * 100));
            });
            onLog(`  ✓ ${part.name} written`);
        }

        onLog('\nVerification skipped (no reference MD5).');
        onLog('Rebooting device...');
        await this.flashEnd(true);
        onLog('\n✅ Flash complete!');
        onProgress(100);
    }
}

// ── Utility ─────────────────────────────────────────────────

function sleep(ms) { return new Promise(r => setTimeout(r, ms)); }

// ── UI Controller ───────────────────────────────────────────

(function () {
    const flasher = new BrambleFlasher();
    const connectBtn   = document.getElementById('connect-btn');
    const flashBtn     = document.getElementById('flash-btn');
    const boardSelect  = document.getElementById('board-select');
    const releaseSelect = document.getElementById('release-select');
    const refreshReleasesBtn = document.getElementById('refresh-releases-btn');
    const progressSection = document.querySelector('.progress-section');
    const progressFill = document.getElementById('progress-fill');
    const progressText = document.getElementById('progress-text');
    const logOutput    = document.getElementById('log-output');
    const releaseDetails = document.getElementById('release-details');

    const OTA_INDEX_URL = '/ota/index.json';
    let releases = [];

    function log(msg) {
        logOutput.textContent += msg + '\n';
        logOutput.scrollTop = logOutput.scrollHeight;
    }

    function setProgress(pct, text) {
        progressSection.hidden = false;
        progressFill.style.width = pct + '%';
        if (text) progressText.textContent = text;
    }

    function currentRelease() {
        const idx = Number(releaseSelect?.value ?? -1);
        if (Number.isInteger(idx) && idx >= 0 && idx < releases.length) return releases[idx];
        return null;
    }

    function renderReleaseOptions() {
        if (!releaseSelect) return;
        releaseSelect.innerHTML = '';
        if (!releases.length) {
            const o = document.createElement('option');
            o.value = '-1';
            o.textContent = 'No releases available';
            releaseSelect.appendChild(o);
            return;
        }
        releases.forEach((r, i) => {
            const o = document.createElement('option');
            const ts = new Date(r.published_at).toLocaleString();
            o.value = String(i);
            o.textContent = `${r.version} (${r.channel || 'unknown'}) — ${ts}`;
            releaseSelect.appendChild(o);
        });
        releaseSelect.value = '0';
    }

    function renderReleaseDetails() {
        if (!releaseDetails) return;

        const selected = currentRelease();
        if (!selected) {
            releaseDetails.innerHTML = '<h4>Release details</h4><div class="warn">No release selected.</div>';
            flashBtn.disabled = !flasher.connected;
            return;
        }

        try {
            const board = boardSelect.value;
            const boardCfg = BrambleFlasher.BOARDS[board];
            const artifactsByFile = window.BrambleReleaseIndex
                .resolveArtifactsForBoardRelease(board, selected, boardCfg);

            const items = boardCfg.partitions.map((p) => {
                const a = artifactsByFile[p.file];
                const shortHash = a.sha256 ? a.sha256.slice(0, 12) : 'n/a';
                return `<li><strong>${p.name}</strong>: ${p.file} (${a.size || 'n/a'} bytes, sha ${shortHash}…)</li>`;
            }).join('');

            releaseDetails.innerHTML = `
                <h4>Release details</h4>
                <div>Version <strong>${selected.version}</strong> • Channel <strong>${selected.channel || 'unknown'}</strong></div>
                <ul>${items}</ul>
            `;

            flashBtn.disabled = !flasher.connected;
        } catch (e) {
            releaseDetails.innerHTML = `<h4>Release details</h4><div class="warn">${e.message}</div>`;
            flashBtn.disabled = true;
        }
    }

    async function loadReleases() {
        try {
            log('Loading releases...');
            const resp = await fetch(OTA_INDEX_URL, { cache: 'no-store' });
            if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
            const data = await resp.json();
            releases = window.BrambleReleaseIndex.normalizeAndSortReleases(data);
            renderReleaseOptions();
            renderReleaseDetails();
            log(`✓ Loaded ${releases.length} release(s)`);
        } catch (e) {
            releases = [];
            renderReleaseOptions();
            renderReleaseDetails();
            log(`⚠ Release index unavailable: ${e.message}`);
        }
    }

    if (!('serial' in navigator)) {
        log('⚠ Web Serial not supported. Use Chrome or Edge.');
        connectBtn.disabled = true;
        return;
    }

    connectBtn.addEventListener('click', async () => {
        if (flasher.connected) {
            await flasher.disconnect();
            connectBtn.textContent = 'Connect Device';
            connectBtn.classList.remove('danger');
            connectBtn.classList.add('primary');
            flashBtn.disabled = true;
            renderReleaseDetails();
            log('Disconnected.');
            return;
        }
        try {
            log('Requesting serial port...');
            await flasher.connect();
            log('✓ Connected to serial port.');
            connectBtn.textContent = 'Disconnect';
            connectBtn.classList.remove('primary');
            connectBtn.classList.add('danger');
            flashBtn.disabled = false;
            renderReleaseDetails();
        } catch (e) {
            log('✗ ' + e.message);
        }
    });

    flashBtn.addEventListener('click', async () => {
        const board = boardSelect.value;
        const selectedRelease = currentRelease();
        flashBtn.disabled = true;
        connectBtn.disabled = true;
        setProgress(0, 'Starting...');

        try {
            let releaseCtx = null;
            if (selectedRelease) {
                const boardCfg = BrambleFlasher.BOARDS[board];
                const artifactsByFile = window.BrambleReleaseIndex
                    .resolveArtifactsForBoardRelease(board, selectedRelease, boardCfg);
                releaseCtx = { release: selectedRelease, artifactsByFile };
                log(`Using release ${selectedRelease.version} (${selectedRelease.channel || 'unknown'})`);
            } else {
                log('No release selected; falling back to local firmware/<board>/ files.');
            }

            await flasher.flashFirmware(
                board,
                releaseCtx,
                msg => log(msg),
                pct => setProgress(pct, `Flashing... ${pct}%`)
            );
            setProgress(100, 'Done!');
        } catch (e) {
            log('✗ Error: ' + e.message);
            setProgress(0, 'Failed');
        } finally {
            flashBtn.disabled = !flasher.connected;
            connectBtn.disabled = false;
        }
    });

    if (refreshReleasesBtn) {
        refreshReleasesBtn.addEventListener('click', () => {
            loadReleases();
        });
    }

    if (boardSelect) {
        boardSelect.addEventListener('change', () => renderReleaseDetails());
    }
    if (releaseSelect) {
        releaseSelect.addEventListener('change', () => renderReleaseDetails());
    }

    loadReleases();
    renderReleaseDetails();
    log('Bramble Web Flasher ready.');
})();
