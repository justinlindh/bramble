import { describe, it, expect, beforeEach, vi } from 'vitest';

describe('active tab persistence', () => {
  beforeEach(() => {
    localStorage.clear();
    vi.resetModules();
  });

  it('loads initial activeTab from localStorage', async () => {
    localStorage.setItem('bramble-active-tab', 'map');
    const { useStore } = await import('../../src/store/index');

    expect(useStore.getState().activeTab).toBe('map');
  });

  it('persists activeTab updates to localStorage', async () => {
    const { useStore } = await import('../../src/store/index');

    useStore.getState().setActiveTab('config');
    expect(localStorage.getItem('bramble-active-tab')).toBe('config');
  });
});
