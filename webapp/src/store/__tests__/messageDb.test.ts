import { beforeEach, describe, expect, it } from 'vitest';
import { messageDb } from '../messageDb';
import { conversationIdForMessage } from '../index';
import type { Message } from '../../types/bramble';

// These tests pin the invariant that the persisted `by-conversation` index is
// keyed through the store's single classifier (conversationIdForMessage), not a
// second hand-rolled copy. Before this was consolidated, messageDb derived DM
// buckets from a local self-address comparison that could disagree with the
// store the UI actually renders.

function msg(over: Partial<Message>): Message {
  return {
    id: Math.random().toString(36).slice(2),
    direction: 'incoming',
    from: 0xa1b2c3d4,
    to: 0x11111111,
    text: 'hi',
    timestampMs: 1000,
    tier: 'normal',
    status: 'delivered',
    ...over,
  };
}

describe('messageDb conversation bucketing', () => {
  beforeEach(async () => {
    await messageDb.open('11111111');
    await messageDb.clearAll();
  });

  it('persists each message under the store classifier bucket', async () => {
    const incomingDm = msg({ direction: 'incoming', from: 0xa1b2c3d4, to: 0x11111111 });
    const channel = msg({ direction: 'incoming', from: 0xa1b2c3d4, to: 0x11111111, channelIndex: 2 });
    const broadcast = msg({ direction: 'incoming', from: 0xa1b2c3d4, to: 0xffffffff });

    await messageDb.saveMessages([incomingDm, channel, broadcast]);

    for (const m of [incomingDm, channel, broadcast]) {
      const bucket = conversationIdForMessage(m);
      const got = await messageDb.getMessages(bucket);
      expect(got.map(x => x.id)).toContain(m.id);
    }
  });

  it('keys an outgoing DM off the destination even when `from` is the 0-means-self sentinel', async () => {
    // A local self-address comparison (from === selfAddr) would file this under
    // dm:0 because `from` is 0; the store's direction-based rule files it under
    // the peer. The persisted index must match the store.
    const outgoing = msg({ direction: 'outgoing', from: 0, to: 0xa1b2c3d4 });
    expect(conversationIdForMessage(outgoing)).toBe('dm:2712847316');

    await messageDb.saveMessage(outgoing);

    const inPeerBucket = await messageDb.getMessages('dm:2712847316');
    expect(inPeerBucket.map(x => x.id)).toContain(outgoing.id);
    const inZeroBucket = await messageDb.getMessages('dm:0');
    expect(inZeroBucket).toHaveLength(0);
  });

  it('round-trips a filtered bucket without leaking other conversations', async () => {
    const a = msg({ direction: 'incoming', from: 0xaaaa, to: 0x11111111 });
    const b = msg({ direction: 'incoming', from: 0xbbbb, to: 0x11111111 });
    await messageDb.saveMessages([a, b]);

    const onlyA = await messageDb.getMessages(conversationIdForMessage(a));
    expect(onlyA.map(x => x.id)).toEqual([a.id]);
  });
});
