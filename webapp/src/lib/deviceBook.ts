import { safeGetItem, safeSetItem, safeRemoveItem } from '../utils/safeLocalStorage';

const BOOK_KEY = 'bramble.devices';
const TOKEN_PREFIX = 'bramble.deviceToken.';

export type SavedDevice = {
  address: string;
  name: string;
  lastIp: string;
  transport: 'wifi' | 'serial' | 'ble';
  remember: boolean;
  lastConnectedAt: number;
  /** BLE identity for reconnecting without the chooser (saved-device path). */
  bleDeviceId?: string;
  bleDeviceName?: string;
};

function readBook(): SavedDevice[] {
  const raw = safeGetItem(BOOK_KEY);
  if (!raw) return [];
  try {
    const parsed = JSON.parse(raw);
    return Array.isArray(parsed) ? (parsed as SavedDevice[]) : [];
  } catch { return []; }
}
function writeBook(devices: SavedDevice[]): void {
  safeSetItem(BOOK_KEY, JSON.stringify(devices));
}

export function listDevices(): SavedDevice[] {
  return readBook().slice().sort((a, b) => b.lastConnectedAt - a.lastConnectedAt);
}

export function upsertDevice(input: {
  address: string; name?: string; lastIp?: string;
  transport: 'wifi' | 'serial' | 'ble'; remember?: boolean; nowMs?: number;
  bleDeviceId?: string; bleDeviceName?: string;
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
    // An omitted remember expresses no intent (a caller whose form has no
    // Remember control, like serial): preserve the entry's flag.
    remember: input.remember ?? existing?.remember ?? false,
    lastConnectedAt: now,
    // Preserve a known BLE identity when a later save omits it.
    bleDeviceId: input.bleDeviceId ?? existing?.bleDeviceId,
    bleDeviceName: input.bleDeviceName ?? existing?.bleDeviceName,
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
  clearDeviceToken(address);
}

export function getDeviceToken(address: string): string {
  return safeGetItem(TOKEN_PREFIX + address)
    ?? safeGetItem(TOKEN_PREFIX + address, sessionStorage)
    ?? '';
}

// Remove a stored token from BOTH storages while keeping the book entry.
// Reconnecting with a blank token and Remember off used to leave the previous
// session's localStorage copy behind, quietly breaking the "leave off on
// shared devices" promise the Remember checkbox makes.
export function clearDeviceToken(address: string): void {
  safeRemoveItem(TOKEN_PREFIX + address);
  safeRemoveItem(TOKEN_PREFIX + address, sessionStorage);
}

export function setDeviceToken(address: string, token: string, remember: boolean): void {
  const key = TOKEN_PREFIX + address;
  if (remember) {
    safeSetItem(key, token);
    safeRemoveItem(key, sessionStorage);
  } else {
    safeSetItem(key, token, sessionStorage);
    safeRemoveItem(key);
  }
}
