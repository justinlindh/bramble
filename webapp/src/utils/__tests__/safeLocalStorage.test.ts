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

  it('targets the passed store (sessionStorage) without touching localStorage', () => {
    safeSetItem('k', 'v', sessionStorage);
    expect(safeGetItem('k', sessionStorage)).toBe('v');
    expect(localStorage.getItem('k')).toBeNull();
    safeRemoveItem('k', sessionStorage);
    expect(safeGetItem('k', sessionStorage)).toBeNull();
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
});
