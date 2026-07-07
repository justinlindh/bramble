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
    transport: null,
    config: null,
    devices: [],
  } as any);
});

describe('message DB namespace on connect', () => {
  it('opens the DB under the connected node address', async () => {
    setNodeAddress(0xf2be6eee);
    await connect('wifi', { url: 'ws://10.0.0.1/ws', ip: '10.0.0.1' });
    expect(messageDb.open).toHaveBeenCalledWith('F2BE6EEE');
  });

  it('uses the NEW node address after switching nodes (no stale snapshot)', async () => {
    // First node: the mock (a distinct address that lingers in the live store).
    setNodeAddress(0x4a555354);
    await connect('websocket');
    expect(messageDb.open).toHaveBeenLastCalledWith('4A555354');

    // Switch to a real node. The DB must open under ITS address, not the mock's.
    (messageDb.open as any).mockClear();
    setNodeAddress(0xf2be6eee);
    await connect('wifi', { url: 'ws://10.0.0.1/ws', ip: '10.0.0.1' });
    expect(messageDb.open).toHaveBeenLastCalledWith('F2BE6EEE');
    expect(messageDb.open).not.toHaveBeenCalledWith('4A555354');
  });
});
