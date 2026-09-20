import { describe, it, expect, beforeEach, vi } from 'vitest';

// Chat, Nodes, and Map read peer names from the store's peerNames map, so the
// user's persisted contact names have to be in it whenever those pages render:
// at load, and after the reset every connect() performs.
describe('peer name hydration', () => {
  const ALICE = 0x1234abcd;
  const BOB = 0x89abcdef;

  beforeEach(() => {
    localStorage.clear();
    vi.resetModules();
  });

  async function loadStoreWith(persisted: string | null) {
    if (persisted !== null) localStorage.setItem('bramble:peerNames', persisted);
    const { useStore } = await import('../../src/store/index');
    return useStore;
  }

  it('seeds peerNames from persisted contact names on load', async () => {
    const useStore = await loadStoreWith(JSON.stringify({ [ALICE]: 'Alice', [BOB]: 'Bob' }));

    expect(useStore.getState().peerNames.get(ALICE)).toBe('Alice');
    expect(useStore.getState().peerNames.get(BOB)).toBe('Bob');
  });

  it('keeps contact names and drops learned ones across resetNodeData', async () => {
    const useStore = await loadStoreWith(JSON.stringify({ [ALICE]: 'Alice' }));
    useStore.getState().setPeerName(BOB, 'learned-from-beacon');

    useStore.getState().resetNodeData();

    expect(useStore.getState().peerNames.get(ALICE)).toBe('Alice');
    expect(useStore.getState().peerNames.has(BOB)).toBe(false);
  });

  it('starts empty when nothing is persisted', async () => {
    const useStore = await loadStoreWith(null);
    expect(useStore.getState().peerNames.size).toBe(0);
  });

  it('tolerates a malformed persisted value', async () => {
    const useStore = await loadStoreWith('not json');
    expect(useStore.getState().peerNames.size).toBe(0);
  });
});
