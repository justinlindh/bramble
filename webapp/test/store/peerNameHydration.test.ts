import { describe, it, expect, beforeEach, vi } from 'vitest';

// The store's peerNames map is the single source Chat, Nodes, and Map read peer
// names from. Before this seeding, it started empty on every load and only the
// Config PeerManager held the persisted copy, so a user-assigned name vanished
// from the rest of the app after a reload until Config was re-opened.
describe('peer name hydration', () => {
  beforeEach(() => {
    localStorage.clear();
    vi.resetModules();
  });

  it('seeds peerNames from persisted contact names on load', async () => {
    localStorage.setItem(
      'bramble:peerNames',
      JSON.stringify({ [0x1234abcd]: 'Alice', [0x89abcdef]: 'Bob' }),
    );
    const { useStore } = await import('../../src/store/index');

    const names = useStore.getState().peerNames;
    expect(names.get(0x1234abcd)).toBe('Alice');
    expect(names.get(0x89abcdef)).toBe('Bob');
  });

  it('starts empty when nothing is persisted', async () => {
    const { useStore } = await import('../../src/store/index');
    expect(useStore.getState().peerNames.size).toBe(0);
  });

  it('tolerates a malformed persisted value', async () => {
    localStorage.setItem('bramble:peerNames', 'not json');
    const { useStore } = await import('../../src/store/index');
    expect(useStore.getState().peerNames.size).toBe(0);
  });
});
