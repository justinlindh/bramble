import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';
import { safeGetItem, safeSetItem, safeRemoveItem } from '../safeLocalStorage';

describe('safeLocalStorage', () => {
  beforeEach(() => {
    localStorage.clear();
    sessionStorage.clear();
  });
  afterEach(() => {
    vi.restoreAllMocks();
  });

  it('reads, writes and removes through localStorage by default', () => {
    expect(safeGetItem('k')).toBeNull();
    safeSetItem('k', 'v');
    expect(safeGetItem('k')).toBe('v');
    expect(localStorage.getItem('k')).toBe('v');
    safeRemoveItem('k');
    expect(safeGetItem('k')).toBeNull();
  });

  it('targets the session area without touching localStorage', () => {
    safeSetItem('k', 'v', 'session');
    expect(safeGetItem('k', 'session')).toBe('v');
    expect(localStorage.getItem('k')).toBeNull();
    safeRemoveItem('k', 'session');
    expect(safeGetItem('k', 'session')).toBeNull();
  });

  it('degrades to null / no-op when the store throws', () => {
    const boom = () => {
      throw new Error('storage disabled');
    };
    vi.spyOn(Storage.prototype, 'getItem').mockImplementation(boom);
    vi.spyOn(Storage.prototype, 'setItem').mockImplementation(boom);
    vi.spyOn(Storage.prototype, 'removeItem').mockImplementation(boom);

    expect(safeGetItem('k')).toBeNull();
    expect(() => safeSetItem('k', 'v')).not.toThrow();
    expect(() => safeRemoveItem('k')).not.toThrow();
  });

  // A block-all-cookies policy makes reading the window.localStorage property
  // throw before any method is called, so resolving the area has to be inside
  // the guard.
  it('degrades when reading the storage property itself throws', () => {
    const denied = () => {
      throw new DOMException('Access is denied for this document.', 'SecurityError');
    };
    vi.spyOn(globalThis, 'localStorage', 'get').mockImplementation(denied);
    vi.spyOn(globalThis, 'sessionStorage', 'get').mockImplementation(denied);

    expect(safeGetItem('k')).toBeNull();
    expect(safeGetItem('k', 'session')).toBeNull();
    expect(() => safeSetItem('k', 'v')).not.toThrow();
    expect(() => safeSetItem('k', 'v', 'session')).not.toThrow();
    expect(() => safeRemoveItem('k')).not.toThrow();
    expect(() => safeRemoveItem('k', 'session')).not.toThrow();
  });
});
