/**
 * Web Storage access that degrades quietly when storage is unavailable.
 *
 * getItem/setItem/removeItem can throw in private-browsing modes, when the
 * quota is exceeded, or when a policy blocks storage. Every persistence site
 * in the app wants the same contract: attempt the access, and on failure fall
 * back to the in-memory default rather than crashing the UI. These helpers own
 * that contract in one place so call sites keep only their own key names and
 * JSON/validation logic.
 *
 * The store defaults to localStorage; pass sessionStorage for the few values
 * (device tokens) that live there. Callers that need to log or otherwise react
 * to a storage failure should keep their own try/catch instead of using these.
 */

export function safeGetItem(key: string, store: Storage = localStorage): string | null {
  try {
    return store.getItem(key);
  } catch {
    return null;
  }
}

export function safeSetItem(key: string, value: string, store: Storage = localStorage): void {
  try {
    store.setItem(key, value);
  } catch {
    /* private mode / quota / blocked: degrade */
  }
}

export function safeRemoveItem(key: string, store: Storage = localStorage): void {
  try {
    store.removeItem(key);
  } catch {
    /* private mode / quota / blocked: degrade */
  }
}
