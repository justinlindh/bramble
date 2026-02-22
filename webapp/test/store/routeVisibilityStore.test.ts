import { describe, it, expect, beforeEach, vi } from 'vitest';

describe('route visibility persistence', () => {
  beforeEach(() => {
    localStorage.clear();
    vi.resetModules();
  });

  it('loads initial showRoutes from localStorage', async () => {
    localStorage.setItem('bramble_show_routes', '1');
    const { useStore } = await import('../../src/store/index');

    expect(useStore.getState().showRoutes).toBe(true);
  });

  it('persists showRoutes updates to localStorage', async () => {
    const { useStore } = await import('../../src/store/index');

    useStore.getState().setShowRoutes(true);
    expect(localStorage.getItem('bramble_show_routes')).toBe('1');

    useStore.getState().setShowRoutes(false);
    expect(localStorage.getItem('bramble_show_routes')).toBe('0');
  });
});
