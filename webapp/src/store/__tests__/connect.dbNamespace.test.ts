import { beforeEach, describe, expect, it, vi } from 'vitest';
import { useStore } from '../index';
import { connect } from '../actions';
import { messageDb } from '../messageDb';

// Regression for cross-device message pollution: connect() must namespace the
// message DB under the address of the node it is CONNECTING TO, read from the
// live store after loadConfig(), not a stale snapshot holding the previous
// node's config. Switching from node A to node B previously reused node A's
// namespace (so A's messages/DMs leaked into B's view).

const rpcMock = vi.fn<(method: string, params?: Record<string, unknown>, timeoutMs?: number) => Promise<any>>();

vi.mock('../../transport', () => {
  class MockBrambleClient {
    subscribe = vi.fn(() => () => {});
    clearSubscriptions = vi.fn();
    disconnect = vi.fn(async () => {});
    rpc = vi.fn((m: string, p?: Record<string, unknown>, t?: number) => rpcMock(m, p, t));
    constructor(_t: unknown) {}
  }
  return {
    createTransport: vi.fn(() => ({
      connect: vi.fn(async () => {}),
      disconnect: vi.fn(async () => {}),
      connected: true,
      onNotification: vi.fn(),
      sendRPC: vi.fn(),
    })),
    BrambleClient: MockBrambleClient,
  };
});

vi.mock('../messageDb', () => ({
  messageDb: {
    open: vi.fn(async () => {}),
    getMessages: vi.fn(async () => []),
    saveMessages: vi.fn(async () => {}),
    saveMessage: vi.fn(async () => {}),
    updateMessageStatus: vi.fn(async () => {}),
  },
}));

vi.mock('../deliveryEventStore', () => ({
  deliveryEventStore: {
    open: vi.fn(async () => {}),
    pruneOldEvents: vi.fn(async () => {}),
    listByMessage: vi.fn(async () => []),
    listByPacketId: vi.fn(async () => []),
    upsertDeliveryEvents: vi.fn(async () => {}),
    upsertDeliveryEvent: vi.fn(async () => {}),
  },
}));

function setNodeAddress(address: number): void {
  rpcMock.mockImplementation(async (method: string) => {
    switch (method) {
      case 'bramble.ping': return { ok: true };
      case 'bramble.getConfig': return { identity: { address } };
      case 'bramble.getVersion': return { supportsDeliveryEventSync: false };
      case 'bramble.getNeighbors': return { neighbors: [] };
      case 'bramble.getRoutes': return { routes: [] };
      case 'bramble.getMessages': return { messages: [] };
      default: return {};
    }
  });
}

beforeEach(() => {
  localStorage.clear();
  sessionStorage.clear();
  vi.clearAllMocks();
  useStore.setState({
    connectionState: 'disconnected',
    connectionError: undefined,
    config: null,
    devices: [],
  } as any);
});

describe('message DB namespace on connect', () => {
  it('opens the DB under the connected node address', async () => {
    setNodeAddress(0xdeadbeef);
    await connect('wifi', { url: 'ws://192.0.2.1/ws', ip: '192.0.2.1' });
    expect(messageDb.open).toHaveBeenCalledWith('DEADBEEF');
  });

  it('uses the NEW node address after switching nodes (no stale snapshot)', async () => {
    // First node: the mock (a distinct address that lingers in the live store).
    setNodeAddress(0x1a2b3c4d);
    await connect('websocket');
    expect(messageDb.open).toHaveBeenLastCalledWith('1A2B3C4D');

    // Switch to a real node. The DB must open under ITS address, not the mock's.
    (messageDb.open as any).mockClear();
    setNodeAddress(0xdeadbeef);
    await connect('wifi', { url: 'ws://192.0.2.1/ws', ip: '192.0.2.1' });
    expect(messageDb.open).toHaveBeenLastCalledWith('DEADBEEF');
    expect(messageDb.open).not.toHaveBeenCalledWith('1A2B3C4D');
  });
});
