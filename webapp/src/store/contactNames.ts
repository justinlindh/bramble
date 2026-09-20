/**
 * Persistent, user-assigned contact names, keyed by peer address.
 *
 * This localStorage map is the durable copy of names the user assigns in the
 * Config PeerManager. It is the single source both PeerManager (which writes it)
 * and the store (which seeds its peerNames map from it on load) read, so the two
 * cannot drift on the key or the on-disk format. The store's in-memory peerNames
 * additionally absorbs live beacon and incoming-message names during a session,
 * but those are transient; only names written here survive a reload.
 */
import { safeGetItem, safeSetItem } from '../utils/safeLocalStorage';

export const CONTACT_NAMES_KEY = 'bramble:peerNames';

/** Load the persisted contact names as an address -> name map. */
export function loadContactNames(): Map<number, string> {
  const raw = safeGetItem(CONTACT_NAMES_KEY);
  if (!raw) return new Map();
  try {
    const obj = JSON.parse(raw) as Record<string, string>;
    return new Map(Object.entries(obj).map(([k, v]) => [Number(k), String(v)]));
  } catch {
    return new Map();
  }
}

/** Persist the contact names map, dropping any blank entries. */
export function saveContactNames(m: Map<number, string>): void {
  const obj: Record<string, string> = {};
  m.forEach((v, k) => {
    if (v) obj[k] = v;
  });
  safeSetItem(CONTACT_NAMES_KEY, JSON.stringify(obj));
}
