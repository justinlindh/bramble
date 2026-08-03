import { beforeEach, describe, expect, it } from 'vitest';
import { useStore } from '../../src/store';
import { messageDb } from '../../src/store/messageDb';
import { deliveryEventStore } from '../../src/store/deliveryEventStore';
import {
  __resetBroadcastTelemetryForTests,
  handleAck,
  handleBroadcastDelivery,
  initMessageStore,
  registerBroadcastSendTelemetry,
} from '../../src/store/actions';
import type { Message } from '../../src/types/bramble';

const NODE_ADDR = 'ABCDEF01';

function makeMessage(overrides: Partial<Message> = {}): Message {
  return {
    id: 'msg-1',
    direction: 'outgoing',
    from: 0,
    to: 0x12345678,
    text: 'hello',
    tier: 'normal',
    timestampMs: Date.now(),
    status: 'sent',
    ...overrides,
  };
}

async function flushAsyncWrites(): Promise<void> {
  await new Promise(resolve => setTimeout(resolve, 0));
}

describe('delivery persistence hydration + live merge', () => {
  beforeEach(async () => {
    useStore.setState({
      connectionState: 'disconnected',
      connectionError: undefined,
      transport: null,
      config: null,
      status: null,
      airtime: null,
      neighbors: [],
      routes: [],
      messages: [],
      conversations: new Map(),
      activeConversationId: 'broadcast',
      activeTab: 'chat',
      showRoutes: false,
      probeResult: null,
      peerNames: new Map(),
      probeCollecting: false,
      peerLocations: [],
      mapFocusAddr: null,
      trafficDebugStatus: null,
      trafficEvents: [],
    });

    await messageDb.open(NODE_ADDR);
    await deliveryEventStore.open(NODE_ADDR);
    await messageDb.clearAll();
    await deliveryEventStore.clearAll();
    __resetBroadcastTelemetryForTests();
  });

  it('hydrates cached message delivery metadata from persisted delivery events', async () => {
    const msg = makeMessage({
      id: 'msg-refresh',
      status: 'sent',
      packetId: 'pkt-1',
      broadcastId: 'bcast-1',
      to: 0xFFFFFFFF,
    });

    await messageDb.saveMessage(msg);
    await deliveryEventStore.upsertDeliveryEvents([
      {
        eventId: 'ack:pkt-1:delivered',
        messageId: msg.id,
        ts: Date.now() - 1000,
        eventType: 'ack',
        payload: { status: 'delivered', relayPath: [{ addr: 0x1111, rssi: -70 }] },
      },
      {
        eventId: 'broadcast:bcast-1:43981',
        messageId: msg.id,
        ts: Date.now(),
        eventType: 'broadcast_delivery',
        payload: { addr: 0xABCD, status: 'delivered', hopCount: 2, deliveredAtMs: Date.now() },
      },
    ]);

    await initMessageStore(NODE_ADDR);

    const hydrated = useStore.getState().messages.find(m => m.id === msg.id);
    expect(hydrated?.status).toBe('delivered');
    expect(hydrated?.relayPath).toEqual([{ addr: 0x1111, rssi: -70 }]);
    expect(hydrated?.broadcastRecipients).toEqual([
      expect.objectContaining({ addr: 0xABCD, status: 'delivered', hopCount: 2 }),
    ]);
  });

  it('persists incoming ack and broadcast delivery events before live merge', async () => {
    const msg = makeMessage({
      id: 'msg-live',
      packetId: 'pkt-live',
      broadcastId: 'bcast-live',
      to: 0xFFFFFFFF,
    });

    useStore.getState().addMessage(msg);
    registerBroadcastSendTelemetry(msg.id, { packetId: 'pkt-live', broadcastId: 'bcast-live' });

    handleAck({ packet_id: 'pkt-live', status: 'delivered', relayPath: [{ addr: 'ABCD', rssi: -81 }] });
    handleBroadcastDelivery({
      broadcast_id: 'bcast-live',
      recipient: 'C0DE',
      status: 'delivered',
      hop_count: 1,
      delivered_at_ms: Date.now(),
    });

    await flushAsyncWrites();

    const events = await deliveryEventStore.listByMessage(msg.id);
    expect(events.some(e => e.eventType === 'ack')).toBe(true);
    expect(events.some(e => e.eventType === 'broadcast_delivery')).toBe(true);

    const updated = useStore.getState().messages.find(m => m.id === msg.id);
    expect(updated?.status).toBe('delivered');
    expect(updated?.broadcastRecipients?.some(r => r.addr === 0xC0DE)).toBe(true);
  });

  it('persists ack and broadcast delivery statuses', async () => {
    const msg = makeMessage({
      id: 'msg-update',
      packetId: 'pkt-update',
      broadcastId: 'bcast-update',
      to: 0xFFFFFFFF,
    });

    useStore.getState().addMessage(msg);
    registerBroadcastSendTelemetry(msg.id, { packetId: 'pkt-update', broadcastId: 'bcast-update' });

    handleAck({
      packet_id: 'pkt-update',
      status: 'delivered',
      relayPath: [{ addr: 'BEEF', rssi: -70 }],
    });

    handleBroadcastDelivery({
      broadcast_id: 'bcast-update',
      recipient: 'CAFE',
      status: 'delivered',
      hop_count: 2,
      delivered_at_ms: Date.now(),
    });

    await flushAsyncWrites();

    const events = await deliveryEventStore.listByMessage(msg.id);
    expect(events.some(e => e.eventType === 'ack')).toBe(true);
    expect(events.some(e => e.eventType === 'broadcast_delivery')).toBe(true);

    const updated = useStore.getState().messages.find(m => m.id === msg.id);
    expect(updated?.status).toBe('delivered');
    expect(updated?.broadcastRecipients?.some(r => r.addr === 0xCAFE)).toBe(true);
  });

  it('prunes expired delivery events on startup retention pass', async () => {
    const msg = makeMessage({ id: 'msg-prune' });
    await messageDb.saveMessage(msg);

    const now = Date.now();
    const thirtyOneDaysMs = 31 * 24 * 60 * 60 * 1000;

    await deliveryEventStore.upsertDeliveryEvents([
      {
        eventId: 'old-event',
        messageId: msg.id,
        ts: now - thirtyOneDaysMs,
        eventType: 'ack',
        payload: { status: 'failed' },
      },
      {
        eventId: 'new-event',
        messageId: msg.id,
        ts: now,
        eventType: 'ack',
        payload: { status: 'delivered' },
      },
    ]);

    await initMessageStore(NODE_ADDR);

    const events = await deliveryEventStore.listByMessage(msg.id);
    expect(events.map(e => e.eventId)).toEqual(['new-event']);
  });
});
