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
 * The area is named rather than passed as a Storage object because reading the
 * window.localStorage/sessionStorage property is itself a throwing operation
 * under a block-all-cookies policy and in sandboxed frames. Resolving it inside
 * the try is what makes the guard cover that case.
 *
 * The area defaults to local; pass 'session' for the few values (device tokens,
 * probe results) that live in sessionStorage. Callers that need to log or
 * otherwise react to a storage failure should keep their own try/catch instead
 * of using these.
 */

export type StorageArea = 'local' | 'session';

function storeFor(area: StorageArea): Storage {
  return area === 'session' ? sessionStorage : localStorage;
}

export function safeGetItem(key: string, area: StorageArea = 'local'): string | null {
  try {
    return storeFor(area).getItem(key);
  } catch {
    return null;
  }
}

export function safeSetItem(key: string, value: string, area: StorageArea = 'local'): void {
  try {
    storeFor(area).setItem(key, value);
  } catch {
    /* private mode / quota / blocked: degrade */
  }
}

export function safeRemoveItem(key: string, area: StorageArea = 'local'): void {
  try {
    storeFor(area).removeItem(key);
  } catch {
    /* private mode / quota / blocked: degrade */
  }
}
