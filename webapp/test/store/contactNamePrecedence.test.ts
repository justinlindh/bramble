import { beforeEach, describe, expect, it, vi } from 'vitest';
import type { Neighbor } from '../../src/types/bramble';

const ALICE = 0x1234abcd;

// setNeighbors reads the firmware's display name off the row; the store's
// Neighbor type does not declare it.
const neighborNamed = (addr: number, name: string) =>
  ({ addr, rssi: -70, snr: 10, lastHeardMs: 1000, name }) as Neighbor;

describe('contact name precedence in the store', () => {
  beforeEach(() => {
    localStorage.clear();
    vi.resetModules();
  });

  async function loadStore() {
    const { useStore } = await import('../../src/store/index');
    return useStore;
  }

  it('keeps a contact name when a neighbor reports its own name', async () => {
    const useStore = await loadStore();
    useStore.getState().setContactName(ALICE, 'Alice');

    useStore.getState().setNeighbors([neighborNamed(ALICE, 'node-7f')]);

    expect(useStore.getState().peerNames.get(ALICE)).toBe('Alice');
  });

  it('keeps a contact name when a message arrives carrying the sender name', async () => {
    const useStore = await loadStore();
    useStore.getState().setContactName(ALICE, 'Alice');

    useStore.getState().learnPeerName(ALICE, 'node-7f');

    expect(useStore.getState().peerNames.get(ALICE)).toBe('Alice');
  });

  it('falls back to the learned name as soon as the contact name is cleared', async () => {
    const useStore = await loadStore();
    useStore.getState().setNeighbors([neighborNamed(ALICE, 'node-7f')]);
    useStore.getState().setContactName(ALICE, 'Alice');

    useStore.getState().setContactName(ALICE, '');

    expect(useStore.getState().peerNames.get(ALICE)).toBe('node-7f');
  });

  it('persists a contact name and removes it when cleared', async () => {
    const useStore = await loadStore();

    useStore.getState().setContactName(ALICE, 'Alice');
    expect(JSON.parse(localStorage.getItem('bramble:peerNames') ?? '{}')).toEqual({ [ALICE]: 'Alice' });

    useStore.getState().setContactName(ALICE, '');
    expect(JSON.parse(localStorage.getItem('bramble:peerNames') ?? '{}')).toEqual({});
  });

  it('keeps a contact name another tab saved since this store loaded', async () => {
    const BOB = 0x89abcdef;
    const useStore = await loadStore();
    localStorage.setItem('bramble:peerNames', JSON.stringify({ [BOB]: 'Bob' }));

    useStore.getState().setContactName(ALICE, 'Alice');

    expect(JSON.parse(localStorage.getItem('bramble:peerNames') ?? '{}')).toEqual({ [ALICE]: 'Alice', [BOB]: 'Bob' });
    expect(useStore.getState().peerNames.get(BOB)).toBe('Bob');
  });

  it('labels a DM conversation with the contact name over a learned one', async () => {
    const useStore = await loadStore();
    useStore.getState().addMessage({
      id: 'm1', direction: 'incoming', from: ALICE, to: 0x0badcafe, text: 'hi',
      tier: 'normal', timestampMs: 1, status: 'delivered',
    });
    useStore.getState().setContactName(ALICE, 'Alice');

    useStore.getState().learnPeerName(ALICE, 'node-7f');

    expect(useStore.getState().conversations.get(`dm:${ALICE}`)?.label).toBe('Alice');
  });
});
