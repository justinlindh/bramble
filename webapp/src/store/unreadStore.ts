/**
 * Persist and restore unread counts across page refreshes.
 * Keyed by node address to support switching between nodes.
 */

const STORAGE_KEY_PREFIX = 'bramble-unreads-';

export interface UnreadCounts {
  [conversationId: string]: number;
}

export function saveUnreadCounts(nodeAddr: string | undefined, counts: UnreadCounts): void {
  if (!nodeAddr) return;
  try {
    const key = `${STORAGE_KEY_PREFIX}${nodeAddr}`;
    localStorage.setItem(key, JSON.stringify(counts));
  } catch (e) {
    // localStorage unavailable (private browsing, quota exceeded, etc.)
    console.warn('[unreadStore] Failed to save unreads:', e);
  }
}

export function loadUnreadCounts(nodeAddr: string | undefined): UnreadCounts {
  if (!nodeAddr) return {};
  try {
    const key = `${STORAGE_KEY_PREFIX}${nodeAddr}`;
    const raw = localStorage.getItem(key);
    if (!raw) return {};
    return JSON.parse(raw) as UnreadCounts;
  } catch (e) {
    console.warn('[unreadStore] Failed to load unreads:', e);
    return {};
  }
}

export function clearUnreadCounts(nodeAddr: string | undefined): void {
  if (!nodeAddr) return;
  try {
    const key = `${STORAGE_KEY_PREFIX}${nodeAddr}`;
    localStorage.removeItem(key);
  } catch (e) {
    console.warn('[unreadStore] Failed to clear unreads:', e);
  }
}
