import { afterEach, describe, expect, it } from 'vitest';
import {
  __clearDeliveryEventSyncStateForTests,
  __normalizeReplayDeliveryEventForTests,
  registerBroadcastSendTelemetry,
} from '../actions';

describe('delivery replay sync glue', () => {
  afterEach(() => {
    __clearDeliveryEventSyncStateForTests();
  });

  it('normalizes snake_case replay payloads', () => {
    const normalized = __normalizeReplayDeliveryEventForTests({
      event_seq: 42,
      event_id: 'evt-42',
      message_id: 'msg-1',
      event_type: 'broadcast_delivery',
      timestamp_ms: 1700000000000,
      payload: { addr: 1234, status: 'delivered', hopCount: 2 },
    });

    expect(normalized).toBeTruthy();
    expect(normalized?.eventId).toBe('evt-42');
    expect(normalized?.messageId).toBe('msg-1');
    expect(normalized?.eventType).toBe('broadcast_delivery');
    expect(normalized?.ts).toBe(1700000000000);
  });

  it('creates deterministic ids when replay event id is missing', () => {
    const normalized = __normalizeReplayDeliveryEventForTests({
      eventSeq: 7,
      messageId: 'abc',
      eventType: 'ack',
      ts: 123,
      payload: { status: 'delivered' },
    });

    expect(normalized).toBeTruthy();
    expect(normalized?.eventId).toBe('replay:7:abc');
  });

  it('resolves messageId from packet_id mapping when message_id is missing', () => {
    registerBroadcastSendTelemetry('msg-ack-1', { packetId: 'AABBCCDD' });

    const normalized = __normalizeReplayDeliveryEventForTests({
      eventSeq: 9,
      eventType: 'ack',
      packet_id: 'AABBCCDD',
      ts: 123,
      payload: { status: 'delivered' },
    });

    expect(normalized).toBeTruthy();
    expect(normalized?.messageId).toBe('msg-ack-1');
  });

  it('drops replay events missing message id', () => {
    const normalized = __normalizeReplayDeliveryEventForTests({
      eventSeq: 8,
      eventType: 'ack',
      ts: 123,
    });

    expect(normalized).toBeNull();
  });
});
