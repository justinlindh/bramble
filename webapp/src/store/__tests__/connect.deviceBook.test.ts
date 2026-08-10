import { beforeEach, describe, expect, it, vi } from 'vitest';
import { useStore } from '../index';
import { connect, saveConnectedDevice } from '../actions';
import { formatAddrHex } from '../../utils/address';
import { listDevices, getDeviceToken } from '../../lib/deviceBook';

// connect() opens a transport and runs RPCs; stub both so we can drive the
// post-connect device-book save/guard without real hardware.
const rpcMock = vi.fn<(
  method: string,
  params?: Record<string, unknown>,
  timeoutMs?: number,
) => Promise<any>>();
const transportConnectMock = vi.fn(async () => {});
const transportDisconnectMock = vi.fn(async () => {});

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
      connect: transportConnectMock,
      disconnect: transportDisconnectMock,
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

function setRpc(address: number | undefined): void {
  rpcMock.mockImplementation(async (method: string) => {
    switch (method) {
      case 'bramble.ping':
        return { ok: true };
      case 'bramble.getConfig':
        return address === undefined ? {} : { identity: { address } };
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
}

beforeEach(() => {
  localStorage.clear();
  sessionStorage.clear();
  vi.clearAllMocks();
  transportConnectMock.mockResolvedValue(undefined);
  transportDisconnectMock.mockResolvedValue(undefined);
  useStore.setState({
    connectionState: 'disconnected',
    connectionError: undefined,
    config: null,
    devices: [],
  } as any);
});

describe('device save on connect', () => {
  it('formats a numeric address to 8-char uppercase hex', () => {
    expect(formatAddrHex(0xDEADBEEF)).toBe('DEADBEEF');
    expect(formatAddrHex(0x0000A001)).toBe('0000A001');
  });

  it('saveConnectedDevice upserts by hex address and stores the token per remember', () => {
    saveConnectedDevice({ addr: 0xDEADBEEF, name: 'V4', ip: '198.51.100.146', token: 'tok', remember: true, transport: 'wifi' });
    const d = listDevices();
    expect(d).toHaveLength(1);
    expect(d[0].address).toBe('DEADBEEF');
    expect(d[0].lastIp).toBe('198.51.100.146');
    expect(getDeviceToken('DEADBEEF')).toBe('tok');
    expect(localStorage.getItem('bramble.deviceToken.DEADBEEF')).toBe('tok'); // remembered
  });

  it('saveConnectedDevice keeps a session-only token when not remembered', () => {
    saveConnectedDevice({ addr: 1, name: 'x', ip: '1.1.1.1', token: 't', remember: false, transport: 'wifi' });
    expect(sessionStorage.getItem('bramble.deviceToken.00000001')).toBe('t');
    expect(localStorage.getItem('bramble.deviceToken.00000001')).toBeNull();
  });
});

describe('connect() device-book guards', () => {
  it('does not book a device when the connected node reports address 0', async () => {
    setRpc(0);
    await connect('wifi', { url: 'ws://127.0.0.1/ws', ip: '192.0.2.5', token: 't', remember: true });
    expect(useStore.getState().connectionState).toBe('connected');
    expect(listDevices()).toHaveLength(0);
  });

  it('books the device on a normal wifi connect once the address is known', async () => {
    setRpc(0xA001);
    await connect('wifi', { url: 'ws://127.0.0.1/ws', ip: '192.0.2.9', token: 'tok', remember: true, name: 'Shed' });
    const d = listDevices();
    expect(d).toHaveLength(1);
    expect(d[0].address).toBe('0000A001');
    expect(d[0].lastIp).toBe('192.0.2.9');
    expect(getDeviceToken('0000A001')).toBe('tok');
  });

  it('disconnects without saving when the node address does not match expectAddressHex', async () => {
    setRpc(0xA001);
    await connect('wifi', {
      url: 'ws://127.0.0.1/ws',
      ip: '192.0.2.9',
      token: 'tok',
      remember: true,
      expectAddressHex: 'CAFEBABE',
    });
    expect(useStore.getState().connectionState).toBe('error');
    expect(listDevices()).toHaveLength(0);
    expect(getDeviceToken('0000A001')).toBe('');
  });
});
