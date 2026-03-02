import { ESPLoader, Transport } from 'esptool-js';
import { buildWifiConfigCommands } from './wifi-config.js';

// Bramble Web Flasher — powered by esptool-js (Espressif official)
// UI controller: connects to ESP32-S3 via Web Serial, flashes firmware from OTA releases

const BOARDS = {
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

(function () {
    // ── State ───────────────────────────────────────────────
    let device = null;      // Web Serial port object
    let transport = null;   // esptool Transport
    let esploader = null;   // ESPLoader instance
    let chip = null;        // detected chip name
    let connected = false;

    // ── DOM refs ────────────────────────────────────────────
    const connectBtn     = document.getElementById('connect-btn');
    const flashBtn       = document.getElementById('flash-btn');
    const buildInfoText  = document.getElementById('build-info');
    const boardSelect    = document.getElementById('board-select');
    const channelSelect  = document.getElementById('channel-select');
    const releaseSelect  = document.getElementById('release-select');
    const progressSection = document.querySelector('.progress-section');
    const progressFill   = document.getElementById('progress-fill');
    const progressText   = document.getElementById('progress-text');
    const releaseDetails = document.getElementById('release-details');
    const statusText     = document.getElementById('status-text');
    const wifiPanel      = document.getElementById('wifi-setup-panel');
    const wifiSsidInput  = document.getElementById('wifi-ssid');
    const wifiPasswordInput = document.getElementById('wifi-password');
    const wifiPasswordToggle = document.getElementById('wifi-password-toggle');
    const wifiEnableCheckbox = document.getElementById('wifi-enable');

    const OTA_INDEX_URL = '/ota/index.json';
    const BOARD_STORAGE_KEY = 'bramble.webflasher.selectedBoard';
    const WIFI_SSID_STORAGE_KEY = 'bramble.webflasher.wifiSsid';

    let releases = [];
    let filteredReleases = [];

    // ── Terminal adapter for esptool-js ─────────────────────
    // esptool-js wants a terminal object with clean/writeLine/write.
    // We log to console and update status text.
    const espTerminal = {
        clean() { /* no-op */ },
        writeLine(data) { console.log('[esptool]', data); },
        write(data) { /* partial line, ignore for UI */ }
    };

    // ── Helpers ─────────────────────────────────────────────
    function setStatus(msg) {
        if (statusText) statusText.textContent = String(msg || '');
    }

    function setProgress(pct, text) {
        progressSection.hidden = false;
        progressFill.style.width = pct + '%';
        if (text) progressText.textContent = text;
    }

    function getSelectedChannel() {
        return channelSelect?.value || 'stable';
    }

    function currentRelease() {
        const idx = Number(releaseSelect?.value ?? -1);
        if (Number.isInteger(idx) && idx >= 0 && idx < filteredReleases.length) return filteredReleases[idx];
        return null;
    }

    function getKnownChannels() {
        return Array.from(new Set(releases.map(r => r.channel || 'stable')));
    }

    function applyChannelFilter() {
        const selected = getSelectedChannel();
        const known = getKnownChannels();
        const fallbackOrder = [selected, 'stable', 'dev', ...known].filter((v, i, arr) => v && arr.indexOf(v) === i);
        filteredReleases = [];
        for (const channel of fallbackOrder) {
            const matches = releases.filter(r => (r.channel || 'stable') === channel);
            if (matches.length) {
                filteredReleases = matches;
                if (channelSelect) channelSelect.value = channel;
                break;
            }
        }
    }

    function renderReleaseOptions() {
        if (!releaseSelect) return;
        releaseSelect.innerHTML = '';
        if (!filteredReleases.length) {
            const o = document.createElement('option');
            o.value = '-1';
            o.textContent = 'No releases available';
            releaseSelect.appendChild(o);
            return;
        }
        filteredReleases.forEach((r, i) => {
            const o = document.createElement('option');
            const dt = new Date(r.published_at);
            const ts = Number.isNaN(dt.getTime()) ? 'unknown date' : dt.toLocaleString();
            o.value = String(i);
            o.textContent = `${r.version} — ${ts}`;
            releaseSelect.appendChild(o);
        });
        releaseSelect.value = '0';
    }

    function renderReleaseDetails() {
        if (!releaseDetails) return;
        const selected = currentRelease();
        if (!selected) {
            releaseDetails.textContent = 'No complete release is available yet.';
            flashBtn.disabled = true;
            return;
        }
        try {
            const board = boardSelect.value;
            const boardCfg = BOARDS[board];
            window.BrambleReleaseIndex.resolveArtifactsForBoardRelease(board, selected, boardCfg);
            releaseDetails.textContent = `Selected ${selected.version} (${selected.channel || 'stable'})`;
            flashBtn.disabled = !connected;
        } catch (e) {
            releaseDetails.textContent = e.message;
            flashBtn.disabled = true;
        }
    }

    async function loadReleases() {
        try {
            setStatus('Loading releases…');
            const resp = await fetch(OTA_INDEX_URL, { cache: 'no-store' });
            if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
            const data = await resp.json();
            releases = window.BrambleReleaseIndex.normalizeAndSortReleases(data);
            applyChannelFilter();
            renderReleaseOptions();
            renderReleaseDetails();
            if (filteredReleases.length) {
                setStatus(`Ready. ${filteredReleases.length} release(s) in ${getSelectedChannel()}.`);
            } else {
                setStatus('No complete releases available yet.');
            }
        } catch {
            releases = [];
            filteredReleases = [];
            renderReleaseOptions();
            renderReleaseDetails();
            setStatus('Release index unavailable.');
        }
    }

    function loadBoardPreference() {
        if (!boardSelect) return;
        const saved = localStorage.getItem(BOARD_STORAGE_KEY);
        if (saved && BOARDS[saved]) boardSelect.value = saved;
    }

    function saveBoardPreference() {
        if (!boardSelect) return;
        localStorage.setItem(BOARD_STORAGE_KEY, boardSelect.value);
    }

    function getWifiConfigSelection() {
        const ssid = String(wifiSsidInput?.value || '').trim();
        const password = String(wifiPasswordInput?.value || '');
        const enabled = Boolean(wifiEnableCheckbox?.checked);
        const panelExpanded = Boolean(wifiPanel?.open);
        return { ssid, password, enabled, panelExpanded };
    }

    function shouldConfigureWifi() {
        const selection = getWifiConfigSelection();
        return selection.enabled && selection.panelExpanded && Boolean(selection.ssid);
    }

    function loadWifiPreference() {
        if (!wifiSsidInput || !wifiEnableCheckbox) return;
        const saved = localStorage.getItem(WIFI_SSID_STORAGE_KEY) || '';
        wifiSsidInput.value = saved;
        wifiEnableCheckbox.checked = Boolean(saved.trim());
    }

    function saveWifiSsidPreference() {
        if (!wifiSsidInput) return;
        const ssid = String(wifiSsidInput.value || '').trim();
        if (ssid) {
            localStorage.setItem(WIFI_SSID_STORAGE_KEY, ssid);
            if (wifiEnableCheckbox) wifiEnableCheckbox.checked = true;
        } else {
            localStorage.removeItem(WIFI_SSID_STORAGE_KEY);
            if (wifiEnableCheckbox) wifiEnableCheckbox.checked = false;
        }
    }

    async function readUntilPrompt(reader, { timeoutMs = 12000, prompt = 'bramble>' } = {}) {
        const decoder = new TextDecoder();
        let buffer = '';
        const timeoutAt = Date.now() + timeoutMs;

        while (Date.now() < timeoutAt) {
            const remaining = timeoutAt - Date.now();
            const readResult = await Promise.race([
                reader.read(),
                new Promise((_, reject) => setTimeout(() => reject(new Error('Timed out waiting for console output.')), Math.max(1, remaining))),
            ]);

            if (readResult.done) {
                throw new Error('Serial console closed unexpectedly.');
            }

            buffer += decoder.decode(readResult.value, { stream: true });
            if (buffer.includes(prompt)) {
                return buffer;
            }
        }

        throw new Error('Timed out waiting for firmware console prompt.');
    }

    async function configureWifiOverSerial({ ssid, password }) {
        if (!device?.readable || !device?.writable) {
            throw new Error('Serial transport unavailable for WiFi configuration.');
        }

        const commands = buildWifiConfigCommands({ ssid, password, rebootAfter: true });
        const encoder = new TextEncoder();
        const reader = device.readable.getReader();
        const writer = device.writable.getWriter();

        try {
            // After a fresh flash + reset, the device needs time to boot.
            // ESP32-S3 boot + firmware init typically takes 3-5 seconds.
            await new Promise((resolve) => setTimeout(resolve, 4000));
            await readUntilPrompt(reader, { timeoutMs: 20000 });

            for (const command of commands) {
                await writer.write(encoder.encode(`${command}\r\n`));
                const response = await readUntilPrompt(reader, { timeoutMs: 10000 });
                console.log(`[wifi-config] ${command}`, response);
            }
        } finally {
            writer.releaseLock();
            reader.releaseLock();
        }
    }

    function cleanup() {
        device = null;
        transport = null;
        esploader = null;
        chip = null;
        connected = false;
    }

    // ── Web Serial check ────────────────────────────────────
    if (!('serial' in navigator)) {
        setStatus('Web Serial not supported. Use Chrome or Edge.');
        connectBtn.disabled = true;
        return;
    }

    // ── Connect / Disconnect ────────────────────────────────
    connectBtn.addEventListener('click', async () => {
        if (connected) {
            // Disconnect
            try {
                if (transport) await transport.disconnect();
            } catch {}
            cleanup();
            connectBtn.textContent = 'Connect Device';
            connectBtn.classList.remove('danger');
            connectBtn.classList.add('primary');
            flashBtn.disabled = true;
            renderReleaseDetails();
            setStatus('Disconnected.');
            return;
        }

        try {
            setStatus('Requesting serial port…');
            device = await navigator.serial.requestPort({
                filters: [
                    { usbVendorId: 0x303A }, // Espressif USB-JTAG
                    { usbVendorId: 0x10C4 }, // CP2102
                    { usbVendorId: 0x1A86 }, // CH340
                ]
            });

            transport = new Transport(device, true);

            setStatus('Connecting to bootloader…');
            esploader = new ESPLoader({
                transport,
                baudrate: 115200,
                terminal: espTerminal,
                romBaudrate: 115200,
            });

            chip = await esploader.main();
            connected = true;

            connectBtn.textContent = 'Disconnect';
            connectBtn.classList.remove('primary');
            connectBtn.classList.add('danger');
            flashBtn.disabled = false;
            renderReleaseDetails();
            setStatus(`Connected: ${chip}. Ready to flash.`);
        } catch (e) {
            cleanup();
            setStatus(`Connection failed: ${e.message || 'unknown error'}`);
        }
    });

    // ── Flash ───────────────────────────────────────────────
    flashBtn.addEventListener('click', async () => {
        const board = boardSelect.value;
        const boardCfg = BOARDS[board];
        const selectedRelease = currentRelease();
        const startedAt = Date.now();

        const wifiSelection = getWifiConfigSelection();
        if (wifiSelection.panelExpanded && wifiSelection.enabled && !wifiSelection.ssid) {
            setStatus('Enter a WiFi SSID or uncheck "Configure WiFi after flashing".');
            return;
        }

        flashBtn.disabled = true;
        connectBtn.disabled = true;
        setProgress(0, 'Starting…');
        setStatus('Flashing in progress…');

        try {
            if (!selectedRelease) {
                throw new Error('No release selected.');
            }
            if (!esploader || !connected) {
                throw new Error('Not connected to device.');
            }

            const artifactsByFile = window.BrambleReleaseIndex
                .resolveArtifactsForBoardRelease(board, selectedRelease, boardCfg);

            // Fetch all partition binaries
            const fileArray = [];
            for (const part of boardCfg.partitions) {
                const artifact = artifactsByFile[part.file];
                const url = artifact.file;
                setStatus(`Fetching ${part.name} (${part.file})…`);
                const resp = await fetch(url);
                if (!resp.ok) throw new Error(`Failed to fetch ${part.file}: HTTP ${resp.status}`);
                const bin = new Uint8Array(await resp.arrayBuffer());
                // esptool-js writeFlash expects binary strings, not Uint8Array
                let data = '';
                for (let i = 0; i < bin.length; i++) data += String.fromCharCode(bin[i]);
                fileArray.push({ data, address: part.offset });
                console.log(`[flasher] ${part.name}: ${data.length} bytes → 0x${part.offset.toString(16)}`);
            }

            setStatus('Flashing firmware…');

            const totalBytes = fileArray.reduce((sum, f) => sum + f.data.length, 0);

            await esploader.writeFlash({
                fileArray,
                flashMode: 'keep',
                flashFreq: 'keep',
                flashSize: 'keep',
                eraseAll: false,
                compress: true,
                reportProgress: (fileIndex, written, total) => {
                    // Calculate overall progress across all files
                    let bytesBeforeThisFile = 0;
                    for (let i = 0; i < fileIndex; i++) {
                        bytesBeforeThisFile += fileArray[i].data.length;
                    }
                    const overallWritten = bytesBeforeThisFile + written;
                    const pct = Math.round((overallWritten / totalBytes) * 100);
                    setProgress(pct, `Flashing… ${pct}%`);
                }
            });

            setStatus('Resetting device…');
            try {
                await esploader.hardReset();
            } catch {
                // Some boards don't support hard reset via serial signals; that's OK
            }

            // Release esptool's hold on the serial port so we can reuse it
            // for WiFi provisioning or future connections.
            try {
                if (transport) await transport.disconnect();
            } catch {
                // Best-effort; port may already be released
            }
            transport = null;
            esploader = null;
            chip = null;
            connected = false;

            if (shouldConfigureWifi()) {
                const { ssid, password } = getWifiConfigSelection();
                setStatus('Configuring WiFi...');
                try {
                    await configureWifiOverSerial({ ssid, password });
                    setProgress(100, 'Done!');
                    setStatus(`✅ WiFi configured! Device is connecting to ${ssid}.`);
                } catch (wifiErr) {
                    setProgress(100, 'Flashed');
                    setStatus(`✅ Flash complete, but WiFi setup failed: ${wifiErr.message || 'unknown error'}`);
                }
            } else {
                setProgress(100, 'Done!');
                setStatus('✅ Flash complete! Device is rebooting.');
            }
        } catch (e) {
            setProgress(0, 'Failed');
            const elapsedSec = Math.round((Date.now() - startedAt) / 1000);
            setStatus(`Flash failed (${elapsedSec}s): ${e.message || 'unknown error'}`);
        } finally {
            flashBtn.disabled = !connected;
            connectBtn.disabled = false;
        }
    });

    // ── Event wiring ────────────────────────────────────────
    if (boardSelect) {
        loadBoardPreference();
        boardSelect.addEventListener('change', () => {
            saveBoardPreference();
            renderReleaseDetails();
        });
    }
    if (wifiSsidInput) {
        loadWifiPreference();
        wifiSsidInput.addEventListener('input', () => saveWifiSsidPreference());
    }
    if (wifiPasswordToggle && wifiPasswordInput) {
        wifiPasswordToggle.addEventListener('click', () => {
            const showing = wifiPasswordInput.type === 'text';
            wifiPasswordInput.type = showing ? 'password' : 'text';
            wifiPasswordToggle.textContent = showing ? 'Show' : 'Hide';
            wifiPasswordToggle.setAttribute('aria-label', showing ? 'Show password' : 'Hide password');
        });
    }
    if (channelSelect) {
        channelSelect.addEventListener('change', () => {
            applyChannelFilter();
            renderReleaseOptions();
            renderReleaseDetails();
        });
    }
    if (releaseSelect) {
        releaseSelect.addEventListener('change', () => renderReleaseDetails());
    }

    // ── Build info ──────────────────────────────────────────
    if (buildInfoText) {
        buildInfoText.textContent = 'Powered by esptool-js v0.5.7';
    }

    loadReleases();
    renderReleaseDetails();
    setStatus('Ready.');
})();
