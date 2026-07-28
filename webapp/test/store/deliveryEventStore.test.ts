import { beforeEach, describe, expect, it } from 'vitest';
import { deliveryEventStore, type DeliveryEventRecord } from '../../src/store/deliveryEventStore';

function makeEvent(overrides: Partial<DeliveryEventRecord> = {}): DeliveryEventRecord {
  return {
    eventId: `evt-${Math.random().toString(16).slice(2)}`,
    messageId: 'msg-1',
    ts: Date.now(),
    eventType: 'ack',
    payload: { status: 'delivered' },
    ...overrides,
  };
}

describe('deliveryEventStore', () => {
  beforeEach(async () => {
    await deliveryEventStore.open('ABCDEF01');
    await deliveryEventStore.clearAll();
  });

  it('upserts and lists by messageId', async () => {
    const older = makeEvent({ eventId: 'evt-older', ts: 1000 });
    const newer = makeEvent({ eventId: 'evt-newer', ts: 2000 });

    await deliveryEventStore.upsertDeliveryEvent(newer);
    await deliveryEventStore.upsertDeliveryEvent(older);

    const events = await deliveryEventStore.listByMessage('msg-1');
    expect(events.map(e => e.eventId)).toEqual(['evt-older', 'evt-newer']);
  });

  it('dedupes by eventId and updates existing fields', async () => {
    await deliveryEventStore.upsertDeliveryEvent(
      makeEvent({ eventId: 'evt-1', ts: 1000, payload: { status: 'failed' } }),
    );

    await deliveryEventStore.upsertDeliveryEvent(
      makeEvent({ eventId: 'evt-1', ts: 1500, payload: { status: 'delivered' } }),
    );

    const events = await deliveryEventStore.listByMessage('msg-1');
    expect(events).toHaveLength(1);
    expect(events[0]?.ts).toBe(1500);
    expect(events[0]?.payload).toEqual({ status: 'delivered' });
  });

  it('lists events by packetId in chronological order', async () => {
    await deliveryEventStore.upsertDeliveryEvent(makeEvent({
      eventId: 'evt-pkt-2',
      messageId: 'msg-2',
      packetId: 'pkt-42',
      ts: 2000,
    }));
    await deliveryEventStore.upsertDeliveryEvent(makeEvent({
      eventId: 'evt-pkt-1',
      messageId: 'msg-1',
      packetId: 'pkt-42',
      ts: 1000,
    }));

    const events = await deliveryEventStore.listByPacketId('pkt-42');
    expect(events.map(e => e.eventId)).toEqual(['evt-pkt-1', 'evt-pkt-2']);
  });

  it('prunes events older than cutoff timestamp', async () => {
    await deliveryEventStore.upsertDeliveryEvent(makeEvent({ eventId: 'evt-1', ts: 1000 }));
    await deliveryEventStore.upsertDeliveryEvent(makeEvent({ eventId: 'evt-2', ts: 3000 }));

    const deleted = await deliveryEventStore.pruneOldEvents(2000);
    expect(deleted).toBe(1);

    const events = await deliveryEventStore.listByMessage('msg-1');
    expect(events.map(e => e.eventId)).toEqual(['evt-2']);
  });
});
