# Web Flasher: Replace Custom Bootloader Protocol with esptool-js

> **For Agent:** REQUIRED SUB-SKILL: Use executing-plans to implement this plan task-by-task.

**Goal:** Replace the hand-rolled SLIP/bootloader sync/flash engine in `web-flasher/flasher.js` with Espressif's official `esptool-js` library to fix persistent bootloader sync failures on ESP32-S3 boards.

**Architecture:** Load `esptool-js` v0.5.7 via CDN bundle (`https://unpkg.com/esptool-js@0.5.7/bundle.js`). The bundle exposes `esptool.ESPLoader`, `esptool.Transport`, etc. as globals. Replace the `BrambleFlasher` class internals (connect/sync/flash) with `esptool-js` Transport + ESPLoader while keeping the existing UI (board select, release picker, progress bar, status text) and `release-index.js` completely unchanged.

**Tech Stack:** Vanilla JS (no build step), esptool-js CDN bundle, Web Serial API

---

### Task 1: Add esptool-js CDN script to index.html

**Files:**
- Modify: `web-flasher/index.html`

**Step 1: Add the CDN bundle script tag before flasher.js**

In `web-flasher/index.html`, add this line before the `<script src="flasher.js">` tag:

```html
<script src="https://unpkg.com/esptool-js@0.5.7/bundle.js"></script>
```

The script order should be:
1. `release-index.js`
2. `esptool-js` bundle
3. `flasher.js`

**Step 2: Verify the file is correct**

Read `web-flasher/index.html` and confirm script order.

**Step 3: Commit**

```bash
git add web-flasher/index.html
git commit -m "web-flasher: add esptool-js CDN bundle"
```

---

### Task 2: Rewrite flasher.js to use esptool-js

**Files:**
- Replace: `web-flasher/flasher.js`

**Context the agent needs:**

The `esptool-js` bundle exposes a global `esptool` object with:
- `esptool.Transport(device, traceEnabled)` — wraps a Web Serial port
- `esptool.ESPLoader({ transport, baudrate, terminal, romBaudrate })` — connects to and identifies the chip
- `esploader.main()` — connect, sync, detect chip. Returns chip name string.
- `esploader.writeFlash({ fileArray, flashMode, flashFreq, flashSize, eraseAll, compress, reportProgress })` — flash firmware
- `esploader.hardReset()` — reset the device after flash

The `FlashOptions.fileArray` is `{ data: Uint8Array, address: number }[]`.

`FlashOptions.reportProgress` signature: `(fileIndex: number, written: number, total: number) => void`

The existing UI elements (IDs) that must be wired up remain unchanged:
- `connect-btn`, `flash-btn`, `board-select`, `channel-select`, `release-select`
- `progress-fill`, `progress-text`, `status-text`, `build-info`, `release-details`

The existing `release-index.js` (exposed as `window.BrambleReleaseIndex`) and all its functions remain unchanged.

The board config with partition offsets remains the same — just simplified since we no longer need the custom SLIP/sync code:

```javascript
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
```

**Step 1: Write the new flasher.js**

Replace `web-flasher/flasher.js` entirely with the following:

```javascript
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

    const OTA_INDEX_URL = '/ota/index.json';
    const BOARD_STORAGE_KEY = 'bramble.webflasher.selectedBoard';

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

            transport = new esptool.Transport(device, true);

            setStatus('Connecting to bootloader…');
            esploader = new esptool.ESPLoader({
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

            // Fetch all partition binaries
            const fileArray = [];
            for (const part of boardCfg.partitions) {
                const artifact = artifactsByFile[part.file];
                const url = artifact.file;
                setStatus(`Fetching ${part.name} (${part.file})…`);
                const resp = await fetch(url);
                if (!resp.ok) throw new Error(`Failed to fetch ${part.file}: HTTP ${resp.status}`);
                const data = new Uint8Array(await resp.arrayBuffer());
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

            setProgress(100, 'Done!');
            setStatus('✅ Flash complete! Device is rebooting.');
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
```

**Step 2: Review the file**

Read `web-flasher/flasher.js` and confirm it's correct.

**Step 3: Commit**

```bash
git add web-flasher/flasher.js
git commit -m "web-flasher: replace custom bootloader protocol with esptool-js

Replaces ~400 lines of hand-rolled SLIP framing and bootloader sync
with Espressif's official esptool-js library (v0.5.7 via CDN).

This fixes persistent bootloader sync timeouts on ESP32-S3 boards
(both USB-JTAG/Serial and CP2102/CH340 bridges) by using the same
proven sync/reset logic as the Python esptool.

Key changes:
- Removed: BrambleFlasher class (custom SLIP, sync, flash commands)
- Added: esptool-js Transport + ESPLoader for connect/sync/flash
- Kept: all UI, board configs, release index, progress reporting
- Flash now uses compressed upload for faster writes
- Chip auto-detection on connect (shows detected chip in status)"
```

---

### Task 3: Update index.html to remove build-info diagnostic cruft

**Files:**
- Modify: `web-flasher/index.html`

**Step 1: Remove the old build-info paragraph**

The `<p id="build-info">` element was used for debugging the custom flasher. It can stay but simplify the initial text:

Change:
```html
<p id="build-info" class="build-info">Build: loading…</p>
```
To:
```html
<p id="build-info" class="build-info"></p>
```

**Step 2: Commit**

```bash
git add web-flasher/index.html
git commit -m "web-flasher: clean up build info placeholder"
```

---

### Task 4: Update README

**Files:**
- Modify: `web-flasher/README.md`

**Step 1: Read and update the README**

Read `web-flasher/README.md`, then update it to reflect:
- Now uses `esptool-js` (Espressif official) instead of custom bootloader protocol
- CDN dependency: `https://unpkg.com/esptool-js@0.5.7/bundle.js`
- No build step required
- Supports ESP32-S3 boards (Heltec V3, T-Deck Plus, Heltec V4)

**Step 2: Commit**

```bash
git add web-flasher/README.md
git commit -m "web-flasher: update README for esptool-js migration"
```

---

### Task 5: Build and deploy the container

**Context:**
- The web flasher is served from `bramble-web-client` container on GPU box (`192.168.1.199`)
- The webapp and web-flasher are both in the same container
- Check the Dockerfile and CI to understand the current build/deploy flow

**Step 1: Find and read the Dockerfile**

```bash
find ~/src/bramble -name "Dockerfile*" | grep -v node_modules
```

Read the relevant Dockerfile to understand how web-flasher files get into the container.

**Step 2: Build the container locally (if possible) or push and let CI handle it**

This depends on what the Dockerfile looks like. The agent should:
1. Read the Dockerfile
2. Determine if web-flasher files are copied in as static assets
3. Build and tag the image
4. Push to registry or trigger deploy

```bash
cd ~/src/bramble
# Push the commits to trigger CI, or build locally
git push origin main
```

**Step 3: Verify deployment**

After deploy, verify the web flasher page loads at `https://app.bramblemesh.org/web-flasher/` and that the esptool-js bundle loads (check browser console for `esptool` global).

**Step 4: Commit any deploy-related fixes**

If any Dockerfile or nginx config changes are needed, commit them.
