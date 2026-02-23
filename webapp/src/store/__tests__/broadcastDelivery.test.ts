import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import { useStore } from '../index';
import {
  __resetBroadcastTelemetryForTests,
  handleBroadcastDelivery,
  registerBroadcastSendTelemetry,
} from '../actions';
import type { Message } from '../../types/bramble';

function makeOutgoingBroadcast(id: string): Message {
  return {
    id,
    direction: 'outgoing',
    from: 0,
    to: 0xffffffff,
    text: 'hello world',
    tier: 'broadcast',
    timestampMs: Date.now(),
    status: 'sending',
  };
}

describe('broadcast delivery store plumbing', () => {
  beforeEach(() => {
    __resetBroadcastTelemetryForTests();
    useStore.setState({ messages: [], conversations: new Map() });
  });

  afterEach(() => {
    __resetBroadcastTelemetryForTests();
  });

  it('broadcast send stores broadcastId', () => {
    const msg = makeOutgoingBroadcast('m1');
    useStore.setState({ messages: [msg] });

    registerBroadcastSendTelemetry(msg.id, {
      packetId: 'A1B2C3D4',
      broadcastId: 'BCAST-1',
    });

    const stored = useStore.getState().messages.find(m => m.id === msg.id);
    expect(stored?.packetId).toBe('A1B2C3D4');
    expect(stored?.broadcastId).toBe('BCAST-1');
  });

  it('onBroadcastDelivery aggregates recipients by broadcastId', () => {
    const msg = makeOutgoingBroadcast('m2');
    useStore.setState({ messages: [msg] });
    registerBroadcastSendTelemetry(msg.id, { broadcastId: 'BCAST-2' });

    handleBroadcastDelivery({
      broadcastId: 'BCAST-2',
      packetId: 'P1',
      from: '04CAAAF8',
      status: 'delivered',
      hopCount: 2,
      deliveredAtMs: 200,
    });
    handleBroadcastDelivery({
      broadcastId: 'BCAST-2',
      packetId: 'P1',
      from: '0000000A',
      status: 'failed',
      hopCount: 3,
      deliveredAtMs: 300,
    });

    const stored = useStore.getState().messages.find(m => m.id === msg.id);
    expect(stored?.broadcastRecipients).toHaveLength(2);
    expect(stored?.broadcastRecipients?.map(r => r.addr).sort((a, b) => a - b)).toEqual([
      0x0000000a,
      0x04caaaf8,
    ]);
  });

  it('handles out-of-order telemetry safely', () => {
    handleBroadcastDelivery({
      broadcastId: 'BCAST-3',
      packetId: 'P3',
      from: '0000000B',
      status: 'delivered',
      hopCount: 1,
      deliveredAtMs: 200,
    });

    const msg = makeOutgoingBroadcast('m3');
    useStore.setState({ messages: [msg] });
    registerBroadcastSendTelemetry(msg.id, { broadcastId: 'BCAST-3', packetId: 'P3' });

    handleBroadcastDelivery({
      broadcastId: 'BCAST-3',
      packetId: 'P3',
      from: '0000000B',
      status: 'failed',
      hopCount: 9,
      deliveredAtMs: 100,
    });

    const stored = useStore.getState().messages.find(m => m.id === msg.id);
    expect(stored?.broadcastRecipients).toHaveLength(1);
    expect(stored?.broadcastRecipients?.[0]).toEqual({
      addr: 0x0000000b,
      status: 'delivered',
      hopCount: 1,
      deliveredAtMs: 200,
    });
  });

  it('accepts firmware snake_case payload with recipient field', () => {
    const msg = makeOutgoingBroadcast('m4');
    useStore.setState({ messages: [msg] });
    registerBroadcastSendTelemetry(msg.id, { broadcastId: 'BCAST-4' });

    handleBroadcastDelivery({
      broadcast_id: 'BCAST-4',
      recipient: '63929F02',
      status: 'delivered',
      rssi_at_dest: -88,
    } as unknown as Record<string, unknown>);

    const stored = useStore.getState().messages.find(m => m.id === msg.id);
    expect(stored?.broadcastRecipients).toHaveLength(1);
    expect(stored?.broadcastRecipients?.[0]?.addr).toBe(0x63929f02);
    expect(stored?.broadcastRecipients?.[0]?.status).toBe('delivered');
  });
});
