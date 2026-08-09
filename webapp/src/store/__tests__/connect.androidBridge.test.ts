import { beforeEach, afterEach, describe, expect, it, vi } from 'vitest';
import { useStore } from '../index';
import { connect } from '../actions';

// connect() opens a transport and runs RPCs; stub both (same harness as
// connect.deviceBook.test.ts) so we can drive the post-connect Android
// bridge notification without real hardware.
const rpcMock = vi.fn<(
  method: string,
  params?: Record<string, unknown>,
  timeoutMs?: number,
) => Promise<any>>();

vi.mock('../../transport', () => {
  class MockBrambleClient {
    subscribe = vi.fn(() => () => {});
    clearSubscriptions = vi.fn();
    disconnect = vi.fn(async () => {});
    rpc = vi.fn((method: string, params?: Record<string, unknown>, timeoutMs?: number) => rpcMock(method, params, timeoutMs));
    constructor(_transport: unknown) {}
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

beforeEach(() => {
  localStorage.clear();
  sessionStorage.clear();
  vi.clearAllMocks();
  rpcMock.mockImplementation(async (method: string) => {
    switch (method) {
      case 'bramble.ping':
        return { ok: true };
      case 'bramble.getConfig':
        return { identity: { address: 0xdeadbeef } };
      case 'bramble.getNeighbors':
        return { neighbors: [] };
      case 'bramble.getRoutes':
        return { routes: [] };
      case 'bramble.getMessages':
        return { messages: [] };
      case 'bramble.getVersion':
        return { supportsDeliveryEventSync: false };
      default:
        return {};
    }
  });
  useStore.setState({
    connectionState: 'disconnected',
    connectionError: undefined,
    config: null,
    devices: [],
  } as any);
});

afterEach(() => {
  vi.unstubAllGlobals();
  delete window.brambleAndroidNative;
});

describe('android native bridge notification on connect', () => {
  it('hands wsUrl and token to the bridge after a wifi connect in the Android shell', async () => {
    vi.stubGlobal('brambleAndroid', true);
    const updateConnection = vi.fn();
    window.brambleAndroidNative = { updateConnection };

    await connect('wifi', { url: 'ws://192.0.2.21/ws', token: 'sekrit', ip: '192.0.2.21' });

    expect(useStore.getState().connectionState).toBe('connected');
    expect(updateConnection).toHaveBeenCalledWith('ws://192.0.2.21/ws', 'sekrit');
  });

  it('passes an empty token when none is set', async () => {
    vi.stubGlobal('brambleAndroid', true);
    const updateConnection = vi.fn();
    window.brambleAndroidNative = { updateConnection };

    await connect('wifi', { url: 'ws://192.0.2.21/ws', ip: '192.0.2.21' });

    expect(updateConnection).toHaveBeenCalledWith('ws://192.0.2.21/ws', '');
  });

  it('does not touch the bridge outside the Android shell', async () => {
    vi.stubGlobal('brambleAndroid', undefined);
    const updateConnection = vi.fn();
    window.brambleAndroidNative = { updateConnection };

    await connect('wifi', { url: 'ws://192.0.2.21/ws', token: 'sekrit', ip: '192.0.2.21' });

    expect(useStore.getState().connectionState).toBe('connected');
    expect(updateConnection).not.toHaveBeenCalled();
  });
});
