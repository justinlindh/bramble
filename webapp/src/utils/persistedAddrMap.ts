import { safeGetItem, safeSetItem } from './safeLocalStorage';

/** Load a localStorage JSON object as an address -> string map; empty when absent or malformed. */
export function loadAddrMap(key: string): Map<number, string> {
  const raw = safeGetItem(key);
  if (!raw) return new Map();
  try {
    const obj = JSON.parse(raw) as Record<string, string>;
    return new Map(Object.entries(obj).map(([k, v]) => [Number(k), String(v)]));
  } catch {
    return new Map();
  }
}

/** Persist an address -> string map, dropping blank entries. */
export function saveAddrMap(key: string, m: Map<number, string>): void {
  const obj: Record<string, string> = {};
  m.forEach((v, k) => {
    if (v) obj[k] = v;
  });
  safeSetItem(key, JSON.stringify(obj));
}
