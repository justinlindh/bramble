const BOOK_KEY = 'bramble.devices';
const TOKEN_PREFIX = 'bramble.deviceToken.';

export type SavedDevice = {
  address: string;
  name: string;
  lastIp: string;
  transport: 'wifi' | 'serial';
  remember: boolean;
  lastConnectedAt: number;
};

function safeGet(store: Storage, key: string): string | null {
  try { return store.getItem(key); } catch { return null; }
}
function safeSet(store: Storage, key: string, value: string): void {
  try { store.setItem(key, value); } catch { /* private mode / quota: degrade */ }
}
function safeRemove(store: Storage, key: string): void {
  try { store.removeItem(key); } catch { /* noop */ }
}

function readBook(): SavedDevice[] {
  const raw = safeGet(localStorage, BOOK_KEY);
  if (!raw) return [];
  try {
    const parsed = JSON.parse(raw);
    return Array.isArray(parsed) ? (parsed as SavedDevice[]) : [];
  } catch { return []; }
}
function writeBook(devices: SavedDevice[]): void {
  safeSet(localStorage, BOOK_KEY, JSON.stringify(devices));
}

export function listDevices(): SavedDevice[] {
  return readBook().slice().sort((a, b) => b.lastConnectedAt - a.lastConnectedAt);
}

export function upsertDevice(input: {
  address: string; name?: string; lastIp?: string;
  transport: 'wifi' | 'serial'; remember: boolean; nowMs?: number;
}): SavedDevice {
  const book = readBook();
  const now = input.nowMs ?? Date.now();
  const existing = book.find(d => d.address === input.address);
  const merged: SavedDevice = {
    address: input.address,
    // Keep an existing (possibly user-set) name; only fall back to the auto name for a brand-new entry.
    name: existing?.name ?? input.name ?? `Node ${input.address}`,
    lastIp: input.lastIp ?? existing?.lastIp ?? '',
    transport: input.transport,
    remember: input.remember,
    lastConnectedAt: now,
  };
  const next = book.filter(d => d.address !== input.address);
  next.push(merged);
  writeBook(next);
  return merged;
}

export function renameDevice(address: string, name: string): void {
  const book = readBook();
  const d = book.find(x => x.address === address);
  if (!d) return;
  d.name = name;
  writeBook(book);
}

export function forgetDevice(address: string): void {
  writeBook(readBook().filter(d => d.address !== address));
  safeRemove(localStorage, TOKEN_PREFIX + address);
  safeRemove(sessionStorage, TOKEN_PREFIX + address);
}

export function getDeviceToken(address: string): string {
  return safeGet(localStorage, TOKEN_PREFIX + address)
    ?? safeGet(sessionStorage, TOKEN_PREFIX + address)
    ?? '';
}

export function setDeviceToken(address: string, token: string, remember: boolean): void {
  const key = TOKEN_PREFIX + address;
  if (remember) {
    safeSet(localStorage, key, token);
    safeRemove(sessionStorage, key);
  } else {
    safeSet(sessionStorage, key, token);
    safeRemove(localStorage, key);
  }
}
