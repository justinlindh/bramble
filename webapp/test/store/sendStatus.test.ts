import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { useStore } from '../../src/store';
import { connect, handleAck, sendMessage } from '../../src/store/actions';
import { messageDb } from '../../src/store/messageDb';
import { deliveryEventStore } from '../../src/store/deliveryEventStore';

// Every test connects with the same mocked identity address, so messageDb
// and deliveryEventStore (real, fake-indexeddb-backed singletons) resolve
// to the SAME on-disk database across test cases unless cleared: without
// this, a message saved by one test's sendMessage() call resurfaces via
// initMessageStore()'s cache hydration on the next test's connect('serial').
const NODE_ADDR_HEX = '11112222';

// sendMessage()/handleAck() drive the send-status state machine through a
// real BrambleClient wrapping a stubbed transport: same harness shape as
// src/store/__tests__/connect.deviceBook.test.ts (createTransport + a
// minimal BrambleClient whose rpc() delegates to a controllable mock).
const rpcMock = vi.fn<(
  method: string,
  params?: Record<string, unknown>,
  timeoutMs?: number,
) => Promise<any>>();

vi.mock('../../src/transport', () => {
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

function defaultRpcHandler(method: string): Promise<any> {
  switch (method) {
    case 'bramble.ping':
      return Promise.resolve({ ok: true });
    case 'bramble.getConfig':
      return Promise.resolve({ identity: { address: 0x11112222 } });
    case 'bramble.getStatus':
      return Promise.resolve({});
    case 'bramble.getAirtime':
      return Promise.resolve({});
    case 'bramble.getNeighbors':
      return Promise.resolve({ neighbors: [] });
    case 'bramble.getRoutes':
      return Promise.resolve({ routes: [] });
    case 'bramble.getMessages':
      return Promise.resolve({ messages: [] });
    case 'bramble.getPeerLocations':
      return Promise.resolve({ peerLocations: [] });
    case 'bramble.getVersion':
      return Promise.resolve({ supportsDeliveryEventSync: false });
    default:
      return Promise.resolve({});
  }
}

function firstMessage() {
  return useStore.getState().messages[0];
}

function findMessage(id: string) {
  return useStore.getState().messages.find(m => m.id === id);
}

describe('send-status machine', () => {
  beforeEach(async () => {
    vi.useRealTimers();
    useStore.setState({
      connectionState: 'disconnected',
      connectionError: undefined,
      manualDisconnect: false,
      transport: null,
      config: null,
      status: null,
      airtime: null,
      airtimePolicy: null,
      neighbors: [],
      routes: [],
      messages: [],
      conversations: new Map(),
      activeConversationId: 'broadcast',
      peerLocations: [],
    } as any);
    rpcMock.mockReset();
    rpcMock.mockImplementation(defaultRpcHandler);

    await messageDb.open(NODE_ADDR_HEX);
    await messageDb.clearAll();
    await deliveryEventStore.open(NODE_ADDR_HEX);
    await deliveryEventStore.clearAll();

    await connect('serial');
    expect(useStore.getState().connectionState).toBe('connected');
  });

  afterEach(() => {
    vi.useRealTimers();
  });

  it('lands the outgoing message as "sent" once the node accepts it', async () => {
    rpcMock.mockImplementation((method: string) => {
      if (method === 'bramble.sendMessage') return Promise.resolve({ packetId: 'PKT-1' });
      return defaultRpcHandler(method);
    });

    await sendMessage(0xAABBCCDD, 'hello there');

    const msgs = useStore.getState().messages;
    expect(msgs).toHaveLength(1);
    expect(msgs[0].status).toBe('sent');
    expect(msgs[0].to).toBe(0xAABBCCDD);
    expect(msgs[0].text).toBe('hello there');
  });

  it('handleAck flips a sent message to delivered and cancels its pending timeout', async () => {
    rpcMock.mockImplementation((method: string) => {
      if (method === 'bramble.sendMessage') return Promise.resolve({ packetId: 'PKT-2' });
      return defaultRpcHandler(method);
    });
    await sendMessage(0xAABBCCDD, 'ack me');
    const msgId = firstMessage().id;

    vi.useFakeTimers();
    handleAck({ packetId: 'PKT-2', status: 'delivered' });
    expect(findMessage(msgId)?.status).toBe('delivered');

    // The pending sent -> timeout timer must have been cancelled by the ack:
    // fast-forward well past its 10s window and confirm delivered sticks.
    await vi.advanceTimersByTimeAsync(15000);
    expect(findMessage(msgId)?.status).toBe('delivered');
  });

  it('a duplicate ack for the same packet id is idempotent (no-op, no throw)', async () => {
    rpcMock.mockImplementation((method: string) => {
      if (method === 'bramble.sendMessage') return Promise.resolve({ packetId: 'PKT-3' });
      return defaultRpcHandler(method);
    });
    await sendMessage(0xAABBCCDD, 'dup ack');
    const msgId = firstMessage().id;

    handleAck({ packetId: 'PKT-3', status: 'delivered' });
    expect(findMessage(msgId)?.status).toBe('delivered');

    // The packetId -> msgId correlation is consumed on first use; a second
    // ack for it must not throw and must not touch the message again.
    expect(() => handleAck({ packetId: 'PKT-3', status: 'delivered' })).not.toThrow();
    expect(findMessage(msgId)?.status).toBe('delivered');
  });

  it('handleAck with a non-delivered status flips a sent message to failed', async () => {
    rpcMock.mockImplementation((method: string) => {
      if (method === 'bramble.sendMessage') return Promise.resolve({ packetId: 'PKT-4' });
      return defaultRpcHandler(method);
    });
    await sendMessage(0xAABBCCDD, 'will fail');
    const msgId = firstMessage().id;

    handleAck({ packetId: 'PKT-4', status: 'failed' });

    expect(findMessage(msgId)?.status).toBe('failed');
  });

  it('flips a still-"sent" message to the client-only "timeout" status when no ack ever arrives', async () => {
    rpcMock.mockImplementation((method: string) => {
      if (method === 'bramble.sendMessage') return Promise.resolve({ packetId: 'PKT-5' });
      return defaultRpcHandler(method);
    });

    vi.useFakeTimers();
    await sendMessage(0xAABBCCDD, 'never acked');
    const msgId = firstMessage().id;
    expect(findMessage(msgId)?.status).toBe('sent');

    // actions.ts's SENT_TO_TIMEOUT_UI_MS is a private 10s constant; firmware
    // 'failed' is a distinct, later state this UI-only timer never asserts.
    await vi.advanceTimersByTimeAsync(10000);

    expect(findMessage(msgId)?.status).toBe('timeout');
  });

  it('flips the message to failed and clears the pending timer when the send RPC itself rejects', async () => {
    rpcMock.mockImplementation((method: string) => {
      if (method === 'bramble.sendMessage') return Promise.reject(new Error('radio busy'));
      return defaultRpcHandler(method);
    });

    await expect(sendMessage(0xAABBCCDD, 'boom')).rejects.toThrow('radio busy');

    const msgs = useStore.getState().messages;
    expect(msgs).toHaveLength(1);
    expect(msgs[0].status).toBe('failed');

    // No lingering timer should later flip this back to 'timeout'.
    vi.useFakeTimers();
    await vi.advanceTimersByTimeAsync(15000);
    expect(useStore.getState().messages[0].status).toBe('failed');
  });

  it('does not arm a pending-ack timeout for a broadcast send (no single ack to wait for)', async () => {
    rpcMock.mockImplementation((method: string) => {
      if (method === 'bramble.sendBroadcast') return Promise.resolve({ broadcastId: 'BCAST-1' });
      return defaultRpcHandler(method);
    });

    vi.useFakeTimers();
    await sendMessage(0xFFFFFFFF, 'broadcast hello');
    const msgId = firstMessage().id;
    expect(findMessage(msgId)?.status).toBe('sent');

    await vi.advanceTimersByTimeAsync(15000);
    // Still 'sent': broadcasts are tracked via broadcastRecipients/telemetry
    // instead, so the client-only sent -> timeout timer must never fire.
    expect(findMessage(msgId)?.status).toBe('sent');
  });
});
