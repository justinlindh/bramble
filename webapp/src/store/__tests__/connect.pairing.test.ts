import { beforeEach, describe, expect, it, vi } from 'vitest';
import { useStore } from '../index';
import { connect } from '../actions';

// First-time BLE pairing raises the OS passkey prompt DURING transport
// connect. The transport reports that window via onPairingStateChange
// (feature-detected structurally, like enableAutoReconnect); connect() must
// mirror it into pairingPending so the overlay can tell the user to go type
// the code, and must never leave a stale true behind on any settle path.

const rpcMock = vi.fn<(method: string, params?: Record<string, unknown>, timeoutMs?: number) => Promise<any>>();
const transportConnectMock = vi.fn(async () => {});
let pairingCb: ((pending: boolean) => void) | null = null;

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
      connect: transportConnectMock,
      disconnect: vi.fn(async () => {}),
      connected: true,
      onNotification: vi.fn(),
      sendRPC: vi.fn(),
      onPairingStateChange: vi.fn((cb: (pending: boolean) => void) => { pairingCb = cb; }),
    })),
    BrambleClient: MockBrambleClient,
  };
});

vi.mock('../messageDb', () => ({
  messageDb: {
    open: vi.fn(async () => {}), getMessages: vi.fn(async () => []),
    saveMessages: vi.fn(async () => {}), saveMessage: vi.fn(async () => {}),
    updateMessageStatus: vi.fn(async () => {}),
  },
}));
vi.mock('../deliveryEventStore', () => ({
  deliveryEventStore: {
    open: vi.fn(async () => {}), pruneOldEvents: vi.fn(async () => {}),
    listByMessage: vi.fn(async () => []), listByPacketId: vi.fn(async () => []),
    upsertDeliveryEvents: vi.fn(async () => {}), upsertDeliveryEvent: vi.fn(async () => {}),
  },
}));

function baseRpc(): void {
  rpcMock.mockImplementation(async (method: string) => {
    switch (method) {
      case 'bramble.ping': return { ok: true };
      case 'bramble.getVersion': return { supportsDeliveryEventSync: false };
      case 'bramble.getConfig': return { identity: { address: 0xdeadbeef } };
      case 'bramble.getStatus': return { address: 'DEADBEEF' };
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
  pairingCb = null;
  transportConnectMock.mockResolvedValue(undefined);
  useStore.setState({
    connectionState: 'disconnected', connectionError: undefined,
    pairingPending: false, config: null, devices: [],
  } as any);
});

describe('setPairingPending store action', () => {
  it('toggles pairingPending', () => {
    expect(useStore.getState().pairingPending).toBe(false);
    useStore.getState().setPairingPending(true);
    expect(useStore.getState().pairingPending).toBe(true);
    useStore.getState().setPairingPending(false);
    expect(useStore.getState().pairingPending).toBe(false);
  });
});

describe('connect() pairing state wiring', () => {
  it('wires onPairingStateChange into the store before the transport opens', async () => {
    baseRpc();
    let release!: () => void;
    transportConnectMock.mockImplementation(() => new Promise<void>(r => { release = r; }));
    const p = connect('ble', { token: 'tok' });
    // Registration happens synchronously after createTransport, so the
    // transport can report the OS prompt that opens mid-connect.
    expect(pairingCb).toBeTruthy();
    pairingCb!(true);
    expect(useStore.getState().pairingPending).toBe(true);
    pairingCb!(false);
    expect(useStore.getState().pairingPending).toBe(false);
    release();
    await p;
    expect(useStore.getState().connectionState).toBe('connected');
    expect(useStore.getState().pairingPending).toBe(false);
  });

  it('resets pairingPending when connect succeeds while pairing was still flagged', async () => {
    baseRpc();
    transportConnectMock.mockImplementation(async () => { pairingCb!(true); });
    await connect('ble', { token: 'tok' });
    expect(useStore.getState().connectionState).toBe('connected');
    expect(useStore.getState().pairingPending).toBe(false);
  });

  it('resets pairingPending and maps the error when pairing fails', async () => {
    baseRpc();
    transportConnectMock.mockImplementation(async () => {
      pairingCb!(true);
      throw new Error('Bluetooth pairing did not complete');
    });
    await connect('ble', {});
    expect(useStore.getState().pairingPending).toBe(false);
    expect(useStore.getState().connectionState).toBe('disconnected');
    expect(useStore.getState().connectionError ?? '').toMatch(/connect again/i);
  });
});
