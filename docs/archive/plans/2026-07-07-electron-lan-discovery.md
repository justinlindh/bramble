# Electron LAN Connect + mDNS Discovery Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the WiFi (LAN WebSocket) transport work in the packaged Electron app and add mDNS node discovery with one-click connect merged with the device book.

**Architecture:** The renderer keeps using the existing `WebSocketTransport` directly to `ws://<ip>/ws` (allowed under `file://`). A capabilities short-circuit in `connectionMode.ts` unblocks the WiFi UI. The Electron main process browses `_bramble._tcp` via `bonjour-service` and streams snapshots to the renderer over IPC. Firmware gains mDNS TXT records (`addr`, `name`) for exact device-book matching.

**Tech Stack:** TypeScript (strict), React 19, Zustand, vitest + testing-library, Electron 41 + electron-vite, bonjour-service, ESP-IDF C firmware.

**Design spec:** `docs/archive/plans/2026-07-07-electron-lan-discovery-design.md`

## Global Constraints

- No em dashes anywhere (code, comments, docs, commit messages). A pre-tool hook rejects them.
- All webapp commands run from `webapp/` unless stated otherwise.
- Firmware build check: `bash scripts/flash.sh local heltec-v3 build` (run from repo root).
- Zero behavior change for web deployments: every renderer change must be gated on Electron detection (`isElectron()` or `window.brambleDesktop` presence).
- The webapp address convention is 8-char uppercase hex (`formatAddrHex`, `webapp/src/utils/address.ts`). All address comparisons in new code use that form.
- Commit after every task on branch `electron-lan-discovery` (already checked out).

---

### Task 1: Capabilities short-circuit in Electron

The WiFi connect UI is disabled because `fetchConnectionCapabilities()` fetches `/api/capabilities`, which does not exist under `file://`. Short-circuit when running in Electron.

**Files:**
- Modify: `webapp/src/lib/connectionMode.ts`
- Test: `webapp/src/lib/__tests__/connectionMode.test.ts` (create)

**Interfaces:**
- Consumes: `isElectron()` from `webapp/src/utils/platform.ts` (reads `globalThis.isElectron`, set by the Electron preload).
- Produces: `fetchConnectionCapabilities()` returns `{ mode: 'local', localLanAllowed: true }` in Electron without any network call. Exported const `ELECTRON_CAPABILITIES` for tests.

- [ ] **Step 1: Write the failing test**

Create `webapp/src/lib/__tests__/connectionMode.test.ts`:

```ts
import { describe, it, expect, vi, afterEach } from 'vitest';
import { fetchConnectionCapabilities, DEFAULT_CAPABILITIES, ELECTRON_CAPABILITIES } from '../connectionMode';

describe('fetchConnectionCapabilities', () => {
  afterEach(() => {
    vi.unstubAllGlobals();
  });

  it('short-circuits to local capabilities in Electron without fetching', async () => {
    vi.stubGlobal('isElectron', true);
    const fetchSpy = vi.fn();
    const caps = await fetchConnectionCapabilities(fetchSpy as unknown as typeof fetch);
    expect(caps).toEqual(ELECTRON_CAPABILITIES);
    expect(caps.mode).toBe('local');
    expect(caps.localLanAllowed).toBe(true);
    expect(fetchSpy).not.toHaveBeenCalled();
  });

  it('falls back to the capabilities fetch outside Electron', async () => {
    vi.stubGlobal('isElectron', undefined);
    const fetchSpy = vi.fn().mockRejectedValue(new Error('no server'));
    const caps = await fetchConnectionCapabilities(fetchSpy as unknown as typeof fetch);
    expect(caps).toEqual(DEFAULT_CAPABILITIES);
    expect(fetchSpy).toHaveBeenCalledTimes(1);
  });
});
```

- [ ] **Step 2: Run test to verify it fails**

Run: `npx vitest run src/lib/__tests__/connectionMode.test.ts`
Expected: FAIL, `ELECTRON_CAPABILITIES` is not exported.

- [ ] **Step 3: Implement the short-circuit**

In `webapp/src/lib/connectionMode.ts`, add the import at the top:

```ts
import { isElectron } from '../utils/platform';
```

Add below `DEFAULT_CAPABILITIES`:

```ts
// Electron loads the renderer from file:// where /api/capabilities does not
// exist. Desktop is always local mode: the renderer may open ws:// LAN
// sockets directly (no mixed-content or PNA restrictions under file://).
export const ELECTRON_CAPABILITIES: ConnectionCapabilities = {
  mode: 'local',
  localLanAllowed: true,
};
```

Change `fetchConnectionCapabilities` to check Electron first:

```ts
export async function fetchConnectionCapabilities(fetchImpl: typeof fetch = fetch): Promise<ConnectionCapabilities> {
  if (isElectron()) return ELECTRON_CAPABILITIES;
  try {
    const res = await fetchImpl('/api/capabilities');
    if (!res.ok) return DEFAULT_CAPABILITIES;
    const body = await res.json();
    return normalizeCapabilities(body);
  } catch {
    return DEFAULT_CAPABILITIES;
  }
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `npx vitest run src/lib/__tests__/connectionMode.test.ts`
Expected: PASS (2 tests).

Run the full unit suite to catch regressions: `npm run test:unit`
Expected: PASS.

- [ ] **Step 5: Typecheck and commit**

Run: `npm run typecheck`
Expected: no errors.

```bash
git add webapp/src/lib/connectionMode.ts webapp/src/lib/__tests__/connectionMode.test.ts
git commit -m "feat(webapp): enable LAN capabilities in electron"
```

---

### Task 2: Discovery core (pure functions) + desktop types

Pure, socket-free logic for turning bonjour service records into `DiscoveredNode`s and maintaining a deduped snapshot. Lives in `src/lib` so the existing vitest setup covers it and both the Electron main process and renderer can import the types.

**Files:**
- Create: `webapp/src/types/desktop.ts`
- Create: `webapp/src/lib/discoveryCore.ts`
- Test: `webapp/src/lib/__tests__/discoveryCore.test.ts` (create)

**Interfaces:**
- Produces (used by Tasks 3, 4, 5):

```ts
// types/desktop.ts
export type DiscoveredNode = {
  addrHex?: string;   // full 8-hex uppercase address from TXT `addr` (newer firmware)
  name?: string;      // node name from TXT `name` (newer firmware)
  hostname: string;   // mDNS hostname without .local, e.g. "bramble-6eee"
  ip: string;         // IPv4
  port: number;
};
export type BrambleDesktopApi = {
  startDiscovery(): void;
  stopDiscovery(): void;
  onDiscovered(cb: (nodes: DiscoveredNode[]) => void): () => void; // returns unsubscribe
};
// global: window.brambleDesktop?: BrambleDesktopApi

// discoveryCore.ts
export type RawService = { host?: string; port?: number; addresses?: string[]; txt?: Record<string, unknown> };
export function serviceToNode(svc: RawService): DiscoveredNode | null;
export function nodeKey(node: DiscoveredNode): string;              // addrHex ?? hostname
export function upsertNode(snapshot: DiscoveredNode[], node: DiscoveredNode): DiscoveredNode[];
export function removeService(snapshot: DiscoveredNode[], svc: RawService): DiscoveredNode[];
```

- [ ] **Step 1: Write the desktop types file**

Create `webapp/src/types/desktop.ts`:

```ts
// Types shared between the Electron main process, preload, and renderer for
// LAN node discovery. The renderer feature-detects window.brambleDesktop;
// it is undefined in web deployments.

export type DiscoveredNode = {
  /** Full 8-hex uppercase node address from mDNS TXT `addr` (newer firmware only). */
  addrHex?: string;
  /** Node name from mDNS TXT `name` (newer firmware only). */
  name?: string;
  /** mDNS hostname without .local, e.g. "bramble-6eee". */
  hostname: string;
  /** IPv4 address to connect to. */
  ip: string;
  port: number;
};

export type BrambleDesktopApi = {
  startDiscovery(): void;
  stopDiscovery(): void;
  /** Subscribes to discovery snapshots. Returns an unsubscribe function. */
  onDiscovered(cb: (nodes: DiscoveredNode[]) => void): () => void;
};

declare global {
  interface Window {
    brambleDesktop?: BrambleDesktopApi;
  }
}
```

- [ ] **Step 2: Write the failing tests**

Create `webapp/src/lib/__tests__/discoveryCore.test.ts`:

```ts
import { describe, it, expect } from 'vitest';
import { serviceToNode, nodeKey, upsertNode, removeService, type RawService } from '../discoveryCore';
import type { DiscoveredNode } from '../../types/desktop';

const fullService: RawService = {
  host: 'bramble-6eee.local',
  port: 80,
  addresses: ['fe80::1', '192.168.1.21'],
  txt: { addr: 'f2be6eee', name: 'Garage' },
};

describe('serviceToNode', () => {
  it('maps a full service with TXT records', () => {
    expect(serviceToNode(fullService)).toEqual({
      addrHex: 'F2BE6EEE',
      name: 'Garage',
      hostname: 'bramble-6eee',
      ip: '192.168.1.21',
      port: 80,
    });
  });

  it('handles old firmware without TXT records', () => {
    const svc: RawService = { host: 'bramble-6eee.local', port: 80, addresses: ['192.168.1.21'] };
    expect(serviceToNode(svc)).toEqual({
      addrHex: undefined,
      name: undefined,
      hostname: 'bramble-6eee',
      ip: '192.168.1.21',
      port: 80,
    });
  });

  it('rejects a malformed TXT addr instead of trusting it', () => {
    const svc: RawService = { ...fullService, txt: { addr: 'nothex!!', name: 'Garage' } };
    expect(serviceToNode(svc)?.addrHex).toBeUndefined();
  });

  it('returns null without an IPv4 address', () => {
    expect(serviceToNode({ host: 'bramble-6eee.local', addresses: ['fe80::1'] })).toBeNull();
    expect(serviceToNode({ host: 'bramble-6eee.local' })).toBeNull();
  });

  it('returns null without a host', () => {
    expect(serviceToNode({ addresses: ['192.168.1.21'] })).toBeNull();
  });

  it('defaults port to 80', () => {
    const svc: RawService = { host: 'bramble-6eee.local', addresses: ['192.168.1.21'] };
    expect(serviceToNode(svc)?.port).toBe(80);
  });
});

describe('snapshot maintenance', () => {
  const node = serviceToNode(fullService) as DiscoveredNode;

  it('keys by full address when present, hostname otherwise', () => {
    expect(nodeKey(node)).toBe('F2BE6EEE');
    expect(nodeKey({ ...node, addrHex: undefined })).toBe('bramble-6eee');
  });

  it('upsert replaces an existing entry instead of duplicating (DHCP renew)', () => {
    const s1 = upsertNode([], node);
    const s2 = upsertNode(s1, { ...node, ip: '192.168.1.99' });
    expect(s2).toHaveLength(1);
    expect(s2[0].ip).toBe('192.168.1.99');
  });

  it('upsert keeps distinct nodes', () => {
    const other: DiscoveredNode = { addrHex: '11112222', hostname: 'bramble-2222', ip: '192.168.1.30', port: 80 };
    expect(upsertNode(upsertNode([], node), other)).toHaveLength(2);
  });

  it('removeService drops the matching entry using TXT addr', () => {
    const s = upsertNode([], node);
    expect(removeService(s, fullService)).toHaveLength(0);
  });

  it('removeService falls back to hostname when the down event has no TXT', () => {
    const bare: DiscoveredNode = { hostname: 'bramble-6eee', ip: '192.168.1.21', port: 80 };
    const s = upsertNode([], bare);
    expect(removeService(s, { host: 'bramble-6eee.local' })).toHaveLength(0);
  });

  it('removeService ignores unmatchable services', () => {
    const s = upsertNode([], node);
    expect(removeService(s, {})).toHaveLength(1);
  });

  it('removeService drops an addrHex-keyed node when the down event has no TXT', () => {
    const s = upsertNode([], node);
    expect(removeService(s, { host: 'bramble-6eee.local' })).toHaveLength(0);
  });
});
```

- [ ] **Step 3: Run tests to verify they fail**

Run: `npx vitest run src/lib/__tests__/discoveryCore.test.ts`
Expected: FAIL, cannot resolve `../discoveryCore`.

- [ ] **Step 4: Implement discoveryCore**

Create `webapp/src/lib/discoveryCore.ts`:

```ts
// Pure logic for LAN node discovery: maps bonjour-service results to
// DiscoveredNode and maintains a deduped snapshot. No sockets, no Electron
// imports; the Electron main process (webapp/electron/discovery.ts) is the
// only runtime consumer.

import type { DiscoveredNode } from '../types/desktop';

/** Subset of a bonjour-service discovery result that we consume. */
export type RawService = {
  host?: string;
  port?: number;
  addresses?: string[];
  txt?: Record<string, unknown>;
};

const IPV4 = /^\d{1,3}(\.\d{1,3}){3}$/;
const ADDR_HEX = /^[0-9a-fA-F]{8}$/;

function txtString(txt: Record<string, unknown> | undefined, key: string): string | undefined {
  const v = txt?.[key];
  return typeof v === 'string' && v.length > 0 ? v : undefined;
}

function bareHostname(host: string): string {
  return host.replace(/\.local\.?$/i, '');
}

export function nodeKey(node: DiscoveredNode): string {
  return node.addrHex ?? node.hostname;
}

/** Maps a bonjour service to a DiscoveredNode. Null when unusable (no host or no IPv4). */
export function serviceToNode(svc: RawService): DiscoveredNode | null {
  if (!svc.host) return null;
  const ip = svc.addresses?.find(a => IPV4.test(a));
  if (!ip) return null;
  const addr = txtString(svc.txt, 'addr');
  return {
    addrHex: addr && ADDR_HEX.test(addr) ? addr.toUpperCase() : undefined,
    name: txtString(svc.txt, 'name'),
    hostname: bareHostname(svc.host),
    ip,
    port: svc.port ?? 80,
  };
}

/** New snapshot with the node added, replacing any entry with the same key. */
export function upsertNode(snapshot: DiscoveredNode[], node: DiscoveredNode): DiscoveredNode[] {
  return [...snapshot.filter(n => nodeKey(n) !== nodeKey(node)), node];
}

/** New snapshot without the service's entry. Matches by full address (TXT)
 *  or hostname: down events often omit TXT records, so a node stored under
 *  its addrHex must still be removable by hostname alone. */
export function removeService(snapshot: DiscoveredNode[], svc: RawService): DiscoveredNode[] {
  const addr = txtString(svc.txt, 'addr');
  const addrKey = addr && ADDR_HEX.test(addr) ? addr.toUpperCase() : null;
  const host = svc.host ? bareHostname(svc.host) : null;
  if (!addrKey && !host) return snapshot;
  return snapshot.filter(n => !(addrKey && n.addrHex === addrKey) && !(host && n.hostname === host));
}
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `npx vitest run src/lib/__tests__/discoveryCore.test.ts`
Expected: PASS (11 tests).

- [ ] **Step 6: Typecheck and commit**

Run: `npm run typecheck`
Expected: no errors.

```bash
git add webapp/src/types/desktop.ts webapp/src/lib/discoveryCore.ts webapp/src/lib/__tests__/discoveryCore.test.ts
git commit -m "feat(webapp): discovery core for mDNS node snapshots"
```

---

### Task 3: Electron main discovery module, IPC wiring, preload bridge

Socket glue around the Task 2 core. No unit tests (the logic was TDD'd in Task 2); the gate is typecheck + electron build.

**Files:**
- Modify: `webapp/package.json` (add dependency)
- Create: `webapp/electron/discovery.ts`
- Modify: `webapp/electron/main.ts`
- Modify: `webapp/electron/preload.ts`

**Interfaces:**
- Consumes: `serviceToNode`, `upsertNode`, `removeService` from `webapp/src/lib/discoveryCore.ts`; `DiscoveredNode` from `webapp/src/types/desktop.ts`.
- Produces: IPC channels `discovery:start`, `discovery:stop` (renderer to main, no payload) and `discovery:update` (main to renderer, payload `DiscoveredNode[]`). Preload exposes `window.brambleDesktop` implementing `BrambleDesktopApi`.

- [ ] **Step 1: Install bonjour-service**

Run: `npm install bonjour-service`
Expected: `bonjour-service` appears under `"dependencies"` in `webapp/package.json` (NOT devDependencies; electron-builder only packages runtime dependencies).

- [ ] **Step 2: Create the discovery module**

Create `webapp/electron/discovery.ts`:

```ts
import { Bonjour, type Browser, type Service } from 'bonjour-service';
import { serviceToNode, upsertNode, removeService } from '../src/lib/discoveryCore';
import type { DiscoveredNode } from '../src/types/desktop';

let bonjour: Bonjour | null = null;
let browser: Browser | null = null;
let snapshot: DiscoveredNode[] = [];

/**
 * Starts browsing _bramble._tcp and pushes a full deduped snapshot on every
 * service up/down event. Discovery is best-effort: multicast can fail on
 * VPN or firewalled networks, in which case the renderer gets an empty
 * snapshot and the UI degrades to manual IP entry.
 */
export function startDiscovery(onUpdate: (nodes: DiscoveredNode[]) => void): void {
  stopDiscovery();
  try {
    // The second argument receives async multicast socket errors (VPNs,
    // firewalls); without it they surface as uncaught exceptions in main.
    bonjour = new Bonjour(undefined, (err: Error) => {
      console.warn('[discovery] mDNS error:', err);
      stopDiscovery();
      onUpdate([]);
    });
    browser = bonjour.find({ type: 'bramble' });
  } catch (err) {
    console.warn('[discovery] mDNS unavailable:', err);
    stopDiscovery();
    onUpdate([]);
    return;
  }
  browser.on('up', (svc: Service) => {
    const node = serviceToNode(svc);
    if (!node) return;
    snapshot = upsertNode(snapshot, node);
    onUpdate(snapshot);
  });
  browser.on('down', (svc: Service) => {
    snapshot = removeService(snapshot, svc);
    onUpdate(snapshot);
  });
  onUpdate(snapshot);
}

export function stopDiscovery(): void {
  try { browser?.stop(); } catch { /* noop */ }
  try { bonjour?.destroy(); } catch { /* noop */ }
  browser = null;
  bonjour = null;
  snapshot = [];
}
```

- [ ] **Step 3: Wire IPC in main.ts**

In `webapp/electron/main.ts`:

Change the electron import (line 1) to include `ipcMain`:

```ts
import { app, BrowserWindow, ipcMain, Menu, session, shell } from 'electron';
```

Add below the existing imports:

```ts
import { startDiscovery, stopDiscovery } from './discovery';
```

In the `app.whenReady().then(() => { ... })` block, after `createWindow();`:

```ts
  ipcMain.on('discovery:start', (event) => {
    startDiscovery((nodes) => {
      if (!event.sender.isDestroyed()) {
        event.sender.send('discovery:update', nodes);
      }
    });
  });
  ipcMain.on('discovery:stop', () => stopDiscovery());
```

In `createWindow()`, extend the existing `closed` handler:

```ts
  mainWindow.on('closed', () => {
    stopDiscovery();
    mainWindow = null;
  });
```

- [ ] **Step 4: Expose the bridge in preload.ts**

Replace the full contents of `webapp/electron/preload.ts` with:

```ts
import { contextBridge, ipcRenderer, type IpcRendererEvent } from 'electron';
import { electronAPI } from '@electron-toolkit/preload';

// Discovery payloads are typed on both ends via src/types/desktop.ts
// (DiscoveredNode). The preload just forwards them opaquely.
const brambleDesktop = {
  startDiscovery: (): void => { ipcRenderer.send('discovery:start'); },
  stopDiscovery: (): void => { ipcRenderer.send('discovery:stop'); },
  onDiscovered: (cb: (nodes: unknown[]) => void): (() => void) => {
    const listener = (_event: IpcRendererEvent, nodes: unknown[]) => cb(nodes);
    ipcRenderer.on('discovery:update', listener);
    return () => { ipcRenderer.removeListener('discovery:update', listener); };
  },
};

if (process.contextIsolated) {
  try {
    contextBridge.exposeInMainWorld('electron', electronAPI);
    contextBridge.exposeInMainWorld('isElectron', true);
    contextBridge.exposeInMainWorld('brambleDesktop', brambleDesktop);
  } catch (error) {
    console.error('Failed to expose electron API:', error);
  }
} else {
  // @ts-expect-error fallback for non-isolated context
  window.electron = electronAPI;
  // @ts-expect-error
  window.isElectron = true;
  // @ts-expect-error
  window.brambleDesktop = brambleDesktop;
}
```

- [ ] **Step 5: Typecheck and build**

Run: `npx tsc -p tsconfig.electron.json`
Expected: no errors.

Run: `npm run build:electron`
Expected: build succeeds; `out/main/index.js` and `out/preload/index.js` produced.

- [ ] **Step 6: Commit**

```bash
git add webapp/package.json webapp/package-lock.json webapp/electron/discovery.ts webapp/electron/main.ts webapp/electron/preload.ts
git commit -m "feat(webapp): mDNS discovery in electron main with IPC bridge"
```

---

### Task 4: Merge discovered nodes with the device book

Pure function turning a discovery snapshot plus the saved-device book into the UI list model.

**Files:**
- Create: `webapp/src/lib/nearbyNodes.ts`
- Test: `webapp/src/lib/__tests__/nearbyNodes.test.ts` (create)

**Interfaces:**
- Consumes: `DiscoveredNode` (Task 2), `SavedDevice` from `webapp/src/lib/deviceBook.ts` (`{ address, name, lastIp, transport, remember, lastConnectedAt }`, address is 8-hex uppercase), `nodeKey` from `discoveryCore.ts`.
- Produces (used by Task 5):

```ts
export type NearbyNode = {
  key: string;          // stable list key (addrHex ?? hostname)
  displayName: string;  // saved name > probable saved name > TXT name > hostname
  ip: string;
  hostname: string;
  addrHex?: string;
  txtName?: string;         // raw TXT name, for prefilling the add-device form
  saved?: SavedDevice;      // exact book match by full address
  probableSaved?: SavedDevice; // heuristic match by 16-bit hostname suffix
};
export function mergeNearby(discovered: DiscoveredNode[], devices: SavedDevice[]): NearbyNode[];
```

- [ ] **Step 1: Write the failing tests**

Create `webapp/src/lib/__tests__/nearbyNodes.test.ts`:

```ts
import { describe, it, expect } from 'vitest';
import { mergeNearby } from '../nearbyNodes';
import type { SavedDevice } from '../deviceBook';
import type { DiscoveredNode } from '../../types/desktop';

const saved = (address: string, name: string): SavedDevice => ({
  address, name, lastIp: '192.168.1.9', transport: 'wifi', remember: true, lastConnectedAt: 1,
});

const disc = (over: Partial<DiscoveredNode>): DiscoveredNode => ({
  hostname: 'bramble-6eee', ip: '192.168.1.21', port: 80, ...over,
});

describe('mergeNearby', () => {
  it('exact-matches by full address and uses the saved name', () => {
    const [n] = mergeNearby(
      [disc({ addrHex: 'F2BE6EEE', name: 'Garage (fw)' })],
      [saved('F2BE6EEE', 'Garage')],
    );
    expect(n.saved?.address).toBe('F2BE6EEE');
    expect(n.probableSaved).toBeUndefined();
    expect(n.displayName).toBe('Garage');
  });

  it('uses the TXT name for unknown nodes', () => {
    const [n] = mergeNearby([disc({ addrHex: '11112222', name: 'Attic' })], []);
    expect(n.saved).toBeUndefined();
    expect(n.displayName).toBe('Attic');
    expect(n.txtName).toBe('Attic');
  });

  it('falls back to hostname when there is no name at all', () => {
    const [n] = mergeNearby([disc({})], []);
    expect(n.displayName).toBe('bramble-6eee');
  });

  it('suffix-matches old firmware (no TXT) against a single book entry', () => {
    const [n] = mergeNearby([disc({})], [saved('F2BE6EEE', 'Garage')]);
    expect(n.probableSaved?.address).toBe('F2BE6EEE');
    expect(n.saved).toBeUndefined();
    expect(n.displayName).toBe('Garage');
  });

  it('does not suffix-match when the suffix is ambiguous', () => {
    const [n] = mergeNearby(
      [disc({})],
      [saved('F2BE6EEE', 'Garage'), saved('AAAA6EEE', 'Attic')],
    );
    expect(n.probableSaved).toBeUndefined();
    expect(n.displayName).toBe('bramble-6eee');
  });

  it('never suffix-matches when TXT addr is present (exact info wins)', () => {
    const [n] = mergeNearby([disc({ addrHex: '11116EEE' })], [saved('F2BE6EEE', 'Garage')]);
    expect(n.saved).toBeUndefined();
    expect(n.probableSaved).toBeUndefined();
  });

  it('sorts exact matches first, then probable, then unknown, alphabetically within groups', () => {
    const nodes = mergeNearby(
      [
        disc({ hostname: 'bramble-0001', ip: '10.0.0.1', name: 'Zeta' }),
        disc({ hostname: 'bramble-6eee', ip: '10.0.0.2' }),
        disc({ hostname: 'bramble-0002', ip: '10.0.0.3', addrHex: 'AAAA0002' }),
      ],
      [saved('F2BE6EEE', 'Garage'), saved('AAAA0002', 'Attic')],
    );
    expect(nodes.map(n => n.displayName)).toEqual(['Attic', 'Garage', 'Zeta']);
  });
});
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `npx vitest run src/lib/__tests__/nearbyNodes.test.ts`
Expected: FAIL, cannot resolve `../nearbyNodes`.

- [ ] **Step 3: Implement mergeNearby**

Create `webapp/src/lib/nearbyNodes.ts`:

```ts
// Merges the mDNS discovery snapshot with the device book into the list
// model rendered by the desktop-only NearbyNodes component.

import type { DiscoveredNode } from '../types/desktop';
import type { SavedDevice } from './deviceBook';
import { nodeKey } from './discoveryCore';

export type NearbyNode = {
  key: string;
  displayName: string;
  ip: string;
  hostname: string;
  addrHex?: string;
  /** Raw TXT-advertised name; prefills the add-device form for unknown nodes. */
  txtName?: string;
  /** Exact device-book match by full address. */
  saved?: SavedDevice;
  /** Heuristic match by 16-bit hostname suffix (firmware without TXT records). */
  probableSaved?: SavedDevice;
};

const HOSTNAME_SUFFIX = /^bramble-([0-9a-fA-F]{4})$/;

export function mergeNearby(discovered: DiscoveredNode[], devices: SavedDevice[]): NearbyNode[] {
  const nodes = discovered.map((d): NearbyNode => {
    const saved = d.addrHex ? devices.find(x => x.address === d.addrHex) : undefined;

    // Old firmware advertises no TXT addr; the hostname carries the low 16
    // bits of the address. Only claim a match when it is unambiguous, and
    // never when TXT gave us the full address (exact info wins).
    let probableSaved: SavedDevice | undefined;
    if (!d.addrHex) {
      const m = d.hostname.match(HOSTNAME_SUFFIX);
      if (m) {
        const suffix = m[1].toUpperCase();
        const matches = devices.filter(x => x.address.slice(4) === suffix);
        if (matches.length === 1) probableSaved = matches[0];
      }
    }

    return {
      key: nodeKey(d),
      displayName: saved?.name ?? probableSaved?.name ?? d.name ?? d.hostname,
      ip: d.ip,
      hostname: d.hostname,
      addrHex: d.addrHex,
      txtName: d.name,
      saved,
      probableSaved,
    };
  });

  const rank = (n: NearbyNode) => (n.saved ? 0 : n.probableSaved ? 1 : 2);
  return nodes.sort((a, b) => rank(a) - rank(b) || a.displayName.localeCompare(b.displayName));
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `npx vitest run src/lib/__tests__/nearbyNodes.test.ts`
Expected: PASS (7 tests).

- [ ] **Step 5: Typecheck and commit**

Run: `npm run typecheck`
Expected: no errors.

```bash
git add webapp/src/lib/nearbyNodes.ts webapp/src/lib/__tests__/nearbyNodes.test.ts
git commit -m "feat(webapp): merge discovered nodes with the device book"
```

---

### Task 5: NearbyNodes UI in the connection overlay

Desktop-only "Nearby nodes" list in the WiFi section. Saved/probable matches one-click connect (reusing the DeviceList flow, including the DHCP guard); unknown nodes prefill the manual form.

**Files:**
- Create: `webapp/src/components/NearbyNodes.tsx`
- Modify: `webapp/src/components/ConnectionOverlay.tsx`
- Modify: `webapp/README.md`
- Test: `webapp/src/components/__tests__/NearbyNodes.test.tsx` (create)

**Interfaces:**
- Consumes: `mergeNearby`/`NearbyNode` (Task 4), `window.brambleDesktop` (Task 3), `getDeviceToken` from `deviceBook.ts`, `buildWifiUrl` from `ConnectionOverlay.tsx`, `connect` from `store/actions.ts` with signature `connect(type, { url, token, ip, remember, name, expectAddressHex })`, store selector `useStore(s => s.devices)`.
- Produces: `NearbyNodes` component with props `{ onPickUnknown: (node: NearbyNode) => void }`. Renders `null` on web (no `window.brambleDesktop`) and when the snapshot is empty.

- [ ] **Step 1: Write the failing component test**

Create `webapp/src/components/__tests__/NearbyNodes.test.tsx`:

```tsx
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { render, screen, act, fireEvent } from '@testing-library/react';
import { NearbyNodes } from '../NearbyNodes';
import { useStore } from '../../store/index';
import type { DiscoveredNode } from '../../types/desktop';
import type { SavedDevice } from '../../lib/deviceBook';

vi.mock('../../store/actions', () => ({ connect: vi.fn() }));
import { connect } from '../../store/actions';

let discoveryCb: ((nodes: DiscoveredNode[]) => void) | null = null;

const garage: SavedDevice = {
  address: 'F2BE6EEE', name: 'Garage', lastIp: '192.168.1.9',
  transport: 'wifi', remember: true, lastConnectedAt: 1,
};

beforeEach(() => {
  window.brambleDesktop = {
    startDiscovery: vi.fn(),
    stopDiscovery: vi.fn(),
    onDiscovered: vi.fn((cb) => { discoveryCb = cb; return () => { discoveryCb = null; }; }),
  };
  useStore.setState({ devices: [] });
  localStorage.clear();
});

afterEach(() => {
  delete window.brambleDesktop;
  vi.clearAllMocks();
});

describe('NearbyNodes', () => {
  it('renders nothing on web (no brambleDesktop bridge)', () => {
    delete window.brambleDesktop;
    const { container } = render(<NearbyNodes onPickUnknown={vi.fn()} />);
    expect(container.firstChild).toBeNull();
  });

  it('starts discovery on mount and stops on unmount', () => {
    const { unmount } = render(<NearbyNodes onPickUnknown={vi.fn()} />);
    expect(window.brambleDesktop!.startDiscovery).toHaveBeenCalledTimes(1);
    unmount();
    expect(window.brambleDesktop!.stopDiscovery).toHaveBeenCalledTimes(1);
  });

  it('one-click connects a saved node with token, current IP, and DHCP guard', () => {
    useStore.setState({ devices: [garage] });
    localStorage.setItem('bramble.deviceToken.F2BE6EEE', 'sekrit');
    render(<NearbyNodes onPickUnknown={vi.fn()} />);
    act(() => discoveryCb!([
      { addrHex: 'F2BE6EEE', name: 'Garage (fw)', hostname: 'bramble-6eee', ip: '192.168.1.21', port: 80 },
    ]));
    fireEvent.click(screen.getByRole('button', { name: /connect to garage/i }));
    expect(connect).toHaveBeenCalledWith('wifi', expect.objectContaining({
      url: 'ws://192.168.1.21/ws',
      token: 'sekrit',
      ip: '192.168.1.21',
      remember: true,
      name: 'Garage',
      expectAddressHex: 'F2BE6EEE',
    }));
  });

  it('hands unknown nodes to onPickUnknown instead of connecting', () => {
    const onPickUnknown = vi.fn();
    render(<NearbyNodes onPickUnknown={onPickUnknown} />);
    act(() => discoveryCb!([
      { addrHex: '11112222', name: 'Attic', hostname: 'bramble-2222', ip: '192.168.1.30', port: 80 },
    ]));
    fireEvent.click(screen.getByRole('button', { name: /connect to attic/i }));
    expect(connect).not.toHaveBeenCalled();
    expect(onPickUnknown).toHaveBeenCalledWith(expect.objectContaining({
      ip: '192.168.1.30', txtName: 'Attic',
    }));
  });

  it('renders nothing while the snapshot is empty', () => {
    const { container } = render(<NearbyNodes onPickUnknown={vi.fn()} />);
    expect(container.firstChild).toBeNull();
  });
});
```

- [ ] **Step 2: Run test to verify it fails**

Run: `npx vitest run src/components/__tests__/NearbyNodes.test.tsx`
Expected: FAIL, cannot resolve `../NearbyNodes`.

- [ ] **Step 3: Implement the component**

Create `webapp/src/components/NearbyNodes.tsx` (reuses the DeviceList styles for visual consistency):

```tsx
import { useEffect, useState } from 'react';
import { useStore } from '../store/index';
import { connect } from '../store/actions';
import { getDeviceToken } from '../lib/deviceBook';
import { mergeNearby, type NearbyNode } from '../lib/nearbyNodes';
import type { DiscoveredNode } from '../types/desktop';
import { buildWifiUrl } from './ConnectionOverlay';
import styles from './DeviceList.module.css';

// Desktop-only: lists nodes found via mDNS on the LAN. Saved nodes (device
// book) one-click connect; unknown nodes prefill the add-device form via
// onPickUnknown. Renders nothing on web, where window.brambleDesktop is
// undefined.
export function NearbyNodes({ onPickUnknown }: { onPickUnknown: (node: NearbyNode) => void }) {
  const devices = useStore(s => s.devices);
  const [discovered, setDiscovered] = useState<DiscoveredNode[]>([]);
  const desktop = window.brambleDesktop;

  useEffect(() => {
    if (!desktop) return;
    const unsubscribe = desktop.onDiscovered(setDiscovered);
    desktop.startDiscovery();
    return () => {
      unsubscribe();
      desktop.stopDiscovery();
    };
  }, [desktop]);

  if (!desktop) return null;
  const nodes = mergeNearby(discovered, devices);
  if (nodes.length === 0) return null;

  const onPick = (n: NearbyNode) => {
    const match = n.saved ?? n.probableSaved;
    if (!match) {
      onPickUnknown(n);
      return;
    }
    const tok = getDeviceToken(match.address);
    const url = buildWifiUrl(n.ip, location.protocol, location.host, tok || undefined);
    connect('wifi', {
      url,
      token: tok || undefined,
      ip: n.ip,
      remember: match.remember,
      name: match.name,
      expectAddressHex: match.address,
    });
  };

  return (
    <div className={styles.book}>
      <h3 className={styles.heading}>Nearby nodes</h3>
      <ul className={styles.list}>
        {nodes.map(n => (
          <li key={n.key} className={styles.row}>
            <button
              type="button"
              className={styles.connectBtn}
              onClick={() => onPick(n)}
              aria-label={`Connect to ${n.displayName}`}
              title={n.probableSaved ? 'Matched by hostname; the address is verified on connect' : undefined}
            >
              <span className={styles.name}>{n.displayName}</span>
              <span className={styles.right}>
                <span className={styles.meta}>{n.ip}</span>
                <span className={styles.chevron} aria-hidden="true" />
              </span>
            </button>
          </li>
        ))}
      </ul>
    </div>
  );
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `npx vitest run src/components/__tests__/NearbyNodes.test.tsx`
Expected: PASS (5 tests). If the store rejects the partial `useStore.setState({ devices: [] })`, check how `webapp/src/components/__tests__/ConnectionOverlay.deviceform.test.tsx` seeds state and mirror that pattern.

- [ ] **Step 5: Mount in ConnectionOverlay**

In `webapp/src/components/ConnectionOverlay.tsx`:

Add the imports:

```ts
import { NearbyNodes } from './NearbyNodes';
import type { NearbyNode } from '../lib/nearbyNodes';
```

At the top of the WiFi settings block (immediately inside `{transportType === 'wifi' && (<div className={styles.wifiInput}>`), add:

```tsx
            <NearbyNodes onPickUnknown={(n: NearbyNode) => {
              setWifiIp(n.ip);
              setWifiName(n.txtName ?? '');
            }} />
```

- [ ] **Step 6: Update the README**

In `webapp/README.md`, in the "Desktop App (Electron)" section, after the platform table, add:

```markdown
### LAN discovery

The desktop app discovers Bramble nodes on the local network via mDNS
(`_bramble._tcp`) and lists them under "Nearby nodes" in the WiFi connect
panel. Nodes running firmware with mDNS TXT records (addr/name) are matched
against the device book for one-click reconnect with the saved token. The
desktop app connects directly to `ws://<node-ip>/ws`; no ws-proxy or unified
server is involved.
```

- [ ] **Step 7: Full test suite, typecheck, commit**

Run: `npm run test:unit`
Expected: PASS.

Run: `npm run typecheck`
Expected: no errors.

```bash
git add webapp/src/components/NearbyNodes.tsx webapp/src/components/__tests__/NearbyNodes.test.tsx webapp/src/components/ConnectionOverlay.tsx webapp/README.md
git commit -m "feat(webapp): nearby nodes list with one-click connect on desktop"
```

---

### Task 6: Firmware mDNS TXT records

Advertise the full node address and name so the desktop app can match discovered nodes exactly. Keep TXT in sync on rename.

**Files:**
- Modify: `main/main.c` (mDNS boot stage, around lines 1064 to 1071)
- Modify: `main/rpc_methods.c` (`handle_set_node_name`, around line 671)

**Interfaces:**
- Consumes: `my_addr` (`uint32_t`, static in main.c), `mesh_get_node_name()` from `main/mesh_task.h` (returns `const char*`, empty string when unset), ESP-IDF `mdns.h` (`mdns_txt_item_t`, `mdns_service_add`, `mdns_service_txt_item_set`).
- Produces: mDNS service `_bramble._tcp` port 80 with TXT `addr=<8-hex uppercase>` and, when a name is set, `name=<node name>`. Rename via `bramble.setNodeName` updates the `name` TXT item live.

- [ ] **Step 1: Add TXT records at service registration**

In `main/main.c`, replace:

```c
                mdns_service_add("Bramble", "_bramble", "_tcp", 80, NULL, 0);
```

with:

```c
                /* TXT records let the desktop app identify nodes before
                 * connecting: addr is the full address (the hostname only
                 * carries the low 16 bits), name is the friendly name. */
                char addr_txt[9];
                snprintf(addr_txt, sizeof(addr_txt), "%08" PRIX32, my_addr);
                const char* node_name = mesh_get_node_name();
                mdns_txt_item_t txt[2] = {
                    { "addr", addr_txt },
                    { "name", node_name },
                };
                size_t txt_count = (node_name != NULL && node_name[0] != '\0') ? 2 : 1;
                mdns_service_add("Bramble", "_bramble", "_tcp", 80, txt, txt_count);
```

Note: `mdns_service_add` copies TXT strings internally, so the stack buffer is safe. `mesh_task.h` is already included by main.c; if the compiler disagrees, add `#include "mesh_task.h"` to the includes.

- [ ] **Step 2: Update TXT on rename**

In `main/rpc_methods.c`:

Add to the include block at the top of the file:

```c
#include "mdns.h"
```

In `handle_set_node_name`, immediately after the existing `mesh_set_node_name(name);` line:

```c
    /* Best-effort: reflect the new name in the mDNS TXT record so discovery
     * shows it without a reboot. Fails harmlessly when mDNS is not running
     * (AP mode / WiFi off). */
    (void)mdns_service_txt_item_set("_bramble", "_tcp", "name", name);
```

- [ ] **Step 3: Build the firmware**

Run from the repo root: `bash scripts/flash.sh local heltec-v3 build`
Expected: build succeeds.

- [ ] **Step 4: Commit**

```bash
git add main/main.c main/rpc_methods.c
git commit -m "feat(firmware): advertise node address and name in mDNS TXT"
```

---

### Task 7: End-to-end verification (manual, requires hardware)

The spike risk (Design section 2) and the full flow can only be proven against a real node. This task is a checklist for a human with a Heltec on the LAN; stop and report if any step fails.

**Files:** none (verification only).

- [ ] **Step 1: Verify direct ws:// from the packaged app (the spike risk)**

```bash
cd webapp && npm run package:linux
```

Run the AppImage from `webapp/release/`. In the connect screen:
- The runtime badge must read "Local LAN" (Task 1 working).
- Select WiFi, enter a live node's IP and token, Connect.

Expected: connection succeeds with no ws-proxy or unified server running. This proves `ws://` from `file://` is allowed. If it is blocked (connection fails instantly with a mixed-content console error in devtools), STOP: the design's fallback (main-process WebSocket behind IPC) kicks in and needs a new plan.

Note: `npm run dev:electron` is NOT sufficient for this step; the dev renderer loads over `http://localhost`, which has different mixed-content behavior than packaged `file://`.

- [ ] **Step 2: Verify firmware TXT records**

Flash the Task 6 firmware to a test node. CAUTION: the V3 Heltec has flash encryption; flash it with `--encrypt` or it bricks (see repo runbooks). Prefer a non-encrypted bench unit.

```bash
avahi-browse -r _bramble._tcp
```

Expected: the node appears with TXT `"addr=<8-hex>"` matching its address, and `"name=<its name>"` when a name is set.

- [ ] **Step 3: Verify discovery and one-click connect**

In the packaged app with the node on the same LAN:
- "Nearby nodes" lists the node with its friendly name.
- For a node already in the device book with a remembered token: click connects directly, no token prompt.
- For an unknown node: click prefills the IP and name in the form; enter the token and connect.

- [ ] **Step 4: Verify rename propagation**

While connected, rename the node (Config page or `bramble.setNodeName`). Re-run `avahi-browse -r _bramble._tcp`.
Expected: TXT `name` shows the new value without rebooting the node.

- [ ] **Step 5: Web regression check**

```bash
cd webapp && npm run test:unit && npm run build
```

Expected: PASS; the hosted web build is unaffected (NearbyNodes renders null, capabilities fetch path unchanged outside Electron).
