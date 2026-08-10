import { ESPLoader, Transport } from 'esptool-js';
import { buildWifiConfigCommands } from './wifi-config.js';
import { parseNetworkKeyInput, networkKeyFingerprint } from './network-key.js';

// Bramble Web Flasher: powered by esptool-js (Espressif official)
// UI controller: connects to ESP32-S3 via Web Serial, flashes firmware from OTA releases
// Wizard flow: Flash → Reset (USB-JTAG only) → Device Setup → Done

const BOARDS = {
    'heltec-v3': {
        name: 'Heltec WiFi LoRa 32 V3',
        chipType: 'ESP32-S3',
        flashSize: '8MB',
        usbType: 'uart-bridge',  // CP2102: DTR/RTS reset works, console on UART
        partitions: [
            { name: 'bootloader',      offset: 0x0000,  file: 'bootloader.bin' },
            { name: 'partition-table',  offset: 0x8000,  file: 'partition-table.bin' },
            // otadata is written as erased bytes (the content of ota_data_initial.bin)
            // so a previously used device boots the fresh app in ota_0 instead of a
            // stale image left in ota_1. All bramble partition tables place otadata
            // at 0xe000, size 0x2000.
            { name: 'ota-data',         offset: 0xe000,  blank: 0x2000 },
            { name: 'firmware',         offset: 0x10000, file: 'bramble.bin' }
        ]
    },
    'tdeck-plus': {
        name: 'LILYGO T-Deck Plus',
        chipType: 'ESP32-S3',
        flashSize: '16MB',
        usbType: 'usb-jtag',    // Native USB-JTAG: no DTR/RTS, console over USB
        partitions: [
            { name: 'bootloader',      offset: 0x0000,  file: 'bootloader.bin' },
            { name: 'partition-table',  offset: 0x8000,  file: 'partition-table.bin' },
            // otadata is written as erased bytes (the content of ota_data_initial.bin)
            // so a previously used device boots the fresh app in ota_0 instead of a
            // stale image left in ota_1. All bramble partition tables place otadata
            // at 0xe000, size 0x2000.
            { name: 'ota-data',         offset: 0xe000,  blank: 0x2000 },
            { name: 'firmware',         offset: 0x10000, file: 'bramble.bin' }
        ]
    },
    'heltec-v4': {
        name: 'Heltec V4',
        chipType: 'ESP32-S3',
        flashSize: '8MB',
        usbType: 'usb-jtag',    // Native USB-JTAG: no DTR/RTS, console over USB
        partitions: [
            { name: 'bootloader',      offset: 0x0000,  file: 'bootloader.bin' },
            { name: 'partition-table',  offset: 0x8000,  file: 'partition-table.bin' },
            // otadata is written as erased bytes (the content of ota_data_initial.bin)
            // so a previously used device boots the fresh app in ota_0 instead of a
            // stale image left in ota_1. All bramble partition tables place otadata
            // at 0xe000, size 0x2000.
            { name: 'ota-data',         offset: 0xe000,  blank: 0x2000 },
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

    // ── DOM refs: Step 1 (Flash) ────────────────────────────
    const stepFlash      = document.getElementById('step-flash');
    const connectBtn     = document.getElementById('connect-btn');
    const flashBtn       = document.getElementById('flash-btn');
    const boardSelect    = document.getElementById('board-select');
    const channelSelect  = document.getElementById('channel-select');
    const releaseSelect  = document.getElementById('release-select');
    const progressSection = document.querySelector('.progress-section');
    const progressFill   = document.getElementById('progress-fill');
    const progressText   = document.getElementById('progress-text');
    const releaseDetails = document.getElementById('release-details');
    const statusText     = document.getElementById('status-text');

    // ── DOM refs: Step 2 (Reset prompt) ────────────────────
    const stepReset         = document.getElementById('step-reset');
    const resetContinueBtn  = document.getElementById('reset-continue-btn');

    // ── DOM refs: Step 3 (Device Setup) ────────────────────
    const stepWifi          = document.getElementById('step-wifi');
    const deviceNameInput   = document.getElementById('device-name');
    const wifiSsidInput     = document.getElementById('wifi-ssid');
    const wifiPasswordInput = document.getElementById('wifi-password');
    const wifiPasswordToggle = document.getElementById('wifi-password-toggle');
    const networkKeyInput   = document.getElementById('network-key');
    const authTokenInput    = document.getElementById('auth-token');
    const authTokenToggle   = document.getElementById('auth-token-toggle');
    const authTokenGenerate = document.getElementById('auth-token-generate');
    const wifiConnectBtn    = document.getElementById('wifi-connect-btn');
    const wifiSkipBtn       = document.getElementById('wifi-skip-btn');
    const wifiStatusText    = document.getElementById('wifi-status-text');

    // ── DOM refs: Step 4 (Done) ─────────────────────────────
    const stepDone          = document.getElementById('step-done');
    const doneTitle         = document.getElementById('done-title');
    const doneMessage       = document.getElementById('done-message');
    const flashAnotherBtn   = document.getElementById('flash-another-btn');

    const OTA_INDEX_URL = '/ota/index.json';
    const BOARD_STORAGE_KEY = 'bramble.webflasher.selectedBoard';
    const WIFI_SSID_STORAGE_KEY = 'bramble.webflasher.wifiSsid';

    let releases = [];
    let filteredReleases = [];

    // ── Terminal adapter for esptool-js ─────────────────────
    const espTerminal = {
        clean() { /* no-op */ },
        writeLine(data) { console.log('[esptool]', data); },
        write(data) { /* partial line, ignore for UI */ }
    };

    // ── Step navigation ─────────────────────────────────────
    function showStep(step) {
        stepFlash.hidden  = step !== 'flash';
        stepReset.hidden  = step !== 'reset';
        stepWifi.hidden   = step !== 'wifi';
        stepDone.hidden   = step !== 'done';
    }

    // ── Helpers ─────────────────────────────────────────────
    function setStatus(msg) {
        if (statusText) statusText.textContent = String(msg || '');
    }

    function setWifiStatus(msg) {
        if (wifiStatusText) wifiStatusText.textContent = String(msg || '');
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
            o.textContent = `${r.version} (${ts})`;
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

    function loadWifiPreference() {
        if (!wifiSsidInput) return;
        const saved = localStorage.getItem(WIFI_SSID_STORAGE_KEY) || '';
        wifiSsidInput.value = saved;
    }

    function saveWifiSsidPreference() {
        if (!wifiSsidInput) return;
        const ssid = String(wifiSsidInput.value || '').trim();
        if (ssid) {
            localStorage.setItem(WIFI_SSID_STORAGE_KEY, ssid);
        } else {
            localStorage.removeItem(WIFI_SSID_STORAGE_KEY);
        }
    }

    // ── Serial WiFi provisioning ────────────────────────────
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

            const chunk = decoder.decode(readResult.value, { stream: true });
            buffer += chunk;
            if (buffer.includes(prompt)) {
                return buffer;
            }
        }

        throw new Error('Timed out waiting for firmware console prompt.');
    }

    async function sendSerialRPC(method, params = {}) {
        if (!device?.readable || !device?.writable) {
            throw new Error('Serial port unavailable. Try reconnecting the USB cable.');
        }

        const encoder = new TextEncoder();
        const decoder = new TextDecoder();
        const reader = device.readable.getReader();
        const writer = device.writable.getWriter();

        try {
            // Flush any pending output
            await writer.write(encoder.encode('\n'));
            await new Promise(r => setTimeout(r, 500));
            // Drain
            while (true) {
                const { value, done } = await Promise.race([
                    reader.read(),
                    new Promise(r => setTimeout(() => r({ value: null, done: true }), 300))
                ]);
                if (done || !value) break;
            }

            const id = Math.floor(Math.random() * 100000);
            const req = JSON.stringify({ jsonrpc: '2.0', id, method, params });
            await writer.write(encoder.encode(req + '\r\n'));

            // Read lines looking for JSON-RPC response
            let buf = '';
            const deadline = Date.now() + 5000;
            while (Date.now() < deadline) {
                const { value, done } = await Promise.race([
                    reader.read(),
                    new Promise(r => setTimeout(() => r({ value: null, done: true }), 500))
                ]);
                if (done && !value) continue;
                if (value) buf += decoder.decode(value);

                // Look for complete JSON lines
                const lines = buf.split('\n');
                for (const line of lines) {
                    const trimmed = line.trim();
                    if (!trimmed.startsWith('{')) continue;
                    try {
                        const msg = JSON.parse(trimmed);
                        if (msg.id === id && 'result' in msg) return msg.result;
                        if (msg.id === id && msg.error) throw new Error(msg.error.message || 'RPC error');
                    } catch (e) {
                        if (e.message && !e.message.includes('JSON')) throw e;
                    }
                }
                buf = lines[lines.length - 1] || '';
            }
            throw new Error(`RPC timeout: ${method}`);
        } finally {
            writer.releaseLock();
            reader.releaseLock();
        }
    }

    async function sendSerialCommands(commands) {
        if (!device?.readable || !device?.writable) {
            throw new Error('Serial port unavailable. Try reconnecting the USB cable.');
        }

        const encoder = new TextEncoder();
        const reader = device.readable.getReader();
        const writer = device.writable.getWriter();

        try {
            // Send a newline to trigger a fresh prompt, then wait for it
            await writer.write(encoder.encode('\n'));
            await new Promise((resolve) => setTimeout(resolve, 2000));
            await readUntilPrompt(reader, { timeoutMs: 15000 });

            for (const command of commands) {
                await writer.write(encoder.encode(`${command}\r\n`));
                await readUntilPrompt(reader, { timeoutMs: 10000 });
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

    function showDone({ title, message }) {
        if (doneTitle) doneTitle.textContent = title || "You're all set!";
        if (doneMessage) {
            // The message interpolates user-typed values (device name, SSID,
            // auth token), so build text nodes plus <br> elements instead of
            // assigning innerHTML, which would reinterpret those values as HTML.
            doneMessage.replaceChildren();
            (message || 'Your Bramble device is ready to go.').split('\n').forEach((line, i) => {
                if (i > 0) doneMessage.append(document.createElement('br'));
                doneMessage.append(line);
            });
        }
        showStep('done');
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

            // Fetch all partition binaries; blank regions are synthesized locally
            const fileArray = [];
            for (const part of boardCfg.partitions) {
                if (part.blank) {
                    fileArray.push({ data: '\xff'.repeat(part.blank), address: part.offset });
                    continue;
                }
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
                    let bytesBeforeThisFile = 0;
                    for (let i = 0; i < fileIndex; i++) {
                        bytesBeforeThisFile += fileArray[i].data.length;
                    }
                    const overallWritten = bytesBeforeThisFile + written;
                    const pct = Math.round((overallWritten / totalBytes) * 100);
                    setProgress(pct, `Flashing… ${pct}%`);
                }
            });

            const isUartBridge = boardCfg.usbType === 'uart-bridge';

            if (isUartBridge) {
                // CP2102/CH340 boards: DTR/RTS can reset the device
                try {
                    await transport.setRTS(true);
                    await new Promise(r => setTimeout(r, 100));
                    await transport.setRTS(false);
                } catch { /* best effort */ }

                // Disconnect esptool transport but keep the port object
                try {
                    if (transport) await transport.disconnect();
                } catch { /* best effort */ }

                transport = null;
                esploader = null;
                chip = null;
                connected = false;

                // Wait for device to reboot, then go straight to setup
                setProgress(100, 'Done!');
                await new Promise(r => setTimeout(r, 3000));
                loadWifiPreference();
                showStep('wifi');
            } else {
                // Native USB-JTAG boards: no DTR/RTS, user must press RST
                try {
                    if (transport) await transport.disconnect();
                } catch { /* best effort */ }

                transport = null;
                esploader = null;
                chip = null;
                connected = false;

                setProgress(100, 'Done!');
                showStep('reset');
            }

        } catch (e) {
            setProgress(0, 'Failed');
            const elapsedSec = Math.round((Date.now() - startedAt) / 1000);
            setStatus(`Flash failed (${elapsedSec}s): ${e.message || 'unknown error'}`);
            flashBtn.disabled = !connected;
            connectBtn.disabled = false;
        }
    });

    // ── Reset: Continue button ────────────────────────────
    resetContinueBtn.addEventListener('click', () => {
        loadWifiPreference();
        showStep('wifi');
    });

    // ── Setup: Save & Connect button ───────────────────────
    wifiConnectBtn.addEventListener('click', async () => {
        const deviceName = String(deviceNameInput?.value || '').trim();
        const ssid = String(wifiSsidInput?.value || '').trim();
        const password = String(wifiPasswordInput?.value || '');

        if (!ssid && !deviceName) {
            setWifiStatus('Enter at least a node name or WiFi network.');
            return;
        }

        saveWifiSsidPreference();
        wifiConnectBtn.disabled = true;
        wifiSkipBtn.disabled = true;
        setWifiStatus('Connecting to device console…');

        try {
            const board = boardSelect.value;
            const boardCfg = BOARDS[board];
            const isUartBridge = boardCfg?.usbType === 'uart-bridge';

            if (isUartBridge && device) {
                // UART bridge boards: port object survives reset, just re-open
                if (!device.readable) {
                    await device.open({ baudRate: 115200 });
                }
            } else {
                // USB-JTAG boards: device re-enumerates after reset, must re-select
                setWifiStatus('Select your device in the browser prompt…');
                try {
                    if (device?.readable || device?.writable) {
                        await device.close().catch(() => {});
                    }
                } catch { /* already closed */ }

                device = await navigator.serial.requestPort({
                    filters: [
                        { usbVendorId: 0x303A },
                        { usbVendorId: 0x10C4 },
                        { usbVendorId: 0x1A86 },
                    ]
                });

                if (!device.readable) {
                    await device.open({ baudRate: 115200 });
                }
            }

            // Build command list
            const commands = [];
            if (deviceName) {
                commands.push(`name ${deviceName}`);
            }
            if (ssid) {
                commands.push(...buildWifiConfigCommands({ ssid, password, rebootAfter: false }));
            }
            // Always reboot at the end if we sent any commands
            if (commands.length > 0) {
                commands.push('reboot');
            }

            // Validate the network key before touching the device, so a typo
            // fails the form rather than leaving a half-configured node.
            const networkKeyRaw = String(networkKeyInput?.value || '').trim();
            let networkKey = null;
            if (networkKeyRaw) {
                try {
                    networkKey = parseNetworkKeyInput(networkKeyRaw);
                } catch (e) {
                    setWifiStatus(e.message);
                    wifiConnectBtn.disabled = false;
                    wifiSkipBtn.disabled = false;
                    return;
                }
            }

            // Set auth token via JSON-RPC if user provided one
            const authToken = String(authTokenInput?.value || '').trim();
            if (authToken && authToken.length < 16) {
                // Firmware enforces a 16-byte entropy floor
                setWifiStatus('Auth token must be at least 16 characters (or blank to auto-generate).');
                return;
            }
            if (authToken) {
                setWifiStatus('Setting auth token…');
                await sendSerialRPC('bramble.setAuthToken', { token: authToken });
            }

            // Provision the network key before the reboot below: this is what
            // takes the node from inert to meshing.
            let networkKeyFp = null;
            if (networkKey) {
                setWifiStatus('Provisioning network key…');
                await sendSerialRPC('bramble.setNetworkKey', { key: networkKey });
                networkKeyFp = await networkKeyFingerprint(networkKey);
            }

            setWifiStatus('Configuring device…');
            if (commands.length > 0) {
                await sendSerialCommands(commands);
            }

            const parts = [];
            if (deviceName) parts.push(`Named "${deviceName}"`);
            if (ssid) parts.push(`connecting to "${ssid}"`);
            if (authToken) parts.push('auth token set');

            const tokenNote = authToken
                ? `\n\nYour auth token: ${authToken}\nSave this: you'll need it to connect wirelessly.`
                : `\n\nNo token entered: the device generates its own on first boot.\nRetrieve it with: bramble pair`;

            // Say plainly whether this node can actually mesh yet. A node that
            // looks "configured" but is silently inert is the worst outcome.
            const keyNote = networkKeyFp
                ? `\n\nNetwork key provisioned (fingerprint ${networkKeyFp}).\nConfirm your other nodes report this same fingerprint.`
                : `\n\nNO NETWORK KEY: this node is UNPROVISIONED and inert, so it will not mesh yet.\nOpen the Bramble web app and use Config -> Network Key to found a network or join one.`;

            showDone({
                title: 'Device Configured!',
                message: `${parts.join(' and ')}. You can now close this page.${tokenNote}${keyNote}`
            });
        } catch (err) {
            setWifiStatus(`Setup failed: ${err.message || 'unknown error'}. You can configure these settings later from the Bramble web app.`);
            wifiConnectBtn.disabled = false;
            wifiSkipBtn.disabled = false;
        }
    });

    // ── WiFi: Skip button ───────────────────────────────────
    wifiSkipBtn.addEventListener('click', () => {
        // Skipping setup leaves the node without a network key, which means it
        // cannot mesh. Say so rather than implying the job is finished.
        showDone({
            title: 'Flash complete: node not yet on a mesh',
            message: 'The firmware is installed, but this node has NO NETWORK KEY, so it is '
                + 'UNPROVISIONED and inert: it will not mesh until you give it one.\n\n'
                + 'Connect to it with the Bramble web app over USB or Bluetooth, then use '
                + 'Config -> Network Key to found a new network or join an existing one. '
                + 'You can set WiFi and an auth token from there too.'
        });
    });

    // ── Done: Flash Another ─────────────────────────────────
    flashAnotherBtn.addEventListener('click', () => {
        cleanup();
        connectBtn.textContent = 'Connect Device';
        connectBtn.classList.remove('danger');
        connectBtn.classList.add('primary');
        flashBtn.disabled = true;
        connectBtn.disabled = false;
        progressSection.hidden = true;
        setStatus('Ready.');
        showStep('flash');
        loadReleases();
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
    if (authTokenToggle && authTokenInput) {
        authTokenToggle.addEventListener('click', () => {
            const showing = authTokenInput.type === 'text';
            authTokenInput.type = showing ? 'password' : 'text';
            authTokenToggle.textContent = showing ? 'Show' : 'Hide';
            authTokenToggle.setAttribute('aria-label', showing ? 'Show token' : 'Hide token');
        });
    }
    if (authTokenGenerate && authTokenInput) {
        authTokenGenerate.addEventListener('click', () => {
            const bytes = new Uint8Array(16);
            crypto.getRandomValues(bytes);
            const hex = Array.from(bytes, b => b.toString(16).padStart(2, '0')).join('');
            authTokenInput.value = hex;
            authTokenInput.type = 'text';  // Show it so user can copy
            if (authTokenToggle) authTokenToggle.textContent = 'Hide';
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

    loadReleases();
    renderReleaseDetails();
    setStatus('Ready.');
})();
