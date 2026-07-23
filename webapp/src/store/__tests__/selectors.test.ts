import { describe, expect, it } from 'vitest';
import { __filterMessages as filterMessages } from '../selectors';
import type { Message } from '../../types/bramble';

const SELF = 0x11111111;
const PEER = 0xa1b2c3d4;

let seq = 0;
function msg(overrides: Partial<Message>): Message {
  return {
    id: `m-${seq++}`,
    direction: 'incoming',
    from: PEER,
    to: SELF,
    text: 'hello',
    timestampMs: 1_000,
    status: 'delivered',
    ...overrides,
  } as Message;
}

// filterMessages routes every message through the single conversation
// classifier, so the message list matches the conversation list and unread
// counts. These pin that a message lands in exactly one bucket.
describe('filterMessages single-bucket routing', () => {
  it('keeps a plain broadcast in the broadcast view only', () => {
    const m = msg({ to: 0xffffffff });
    expect(filterMessages([m], 'broadcast')).toEqual([m]);
    expect(filterMessages([m], 'ch:2')).toEqual([]);
  });

  it('files a channel message under its channel, not broadcast', () => {
    const m = msg({ to: SELF, channelIndex: 2 });
    expect(filterMessages([m], 'ch:2')).toEqual([m]);
    expect(filterMessages([m], 'broadcast')).toEqual([]);
  });

  it('files a channel message addressed to the broadcast address under its channel alone', () => {
    const m = msg({ to: 0xffffffff, channelIndex: 2 });
    expect(filterMessages([m], 'ch:2')).toEqual([m]);
    expect(filterMessages([m], 'broadcast')).toEqual([]);
  });

  it('files an incoming DM under its peer bucket', () => {
    const m = msg({ direction: 'incoming', from: PEER, to: SELF });
    expect(filterMessages([m], `dm:${PEER}`)).toEqual([m]);
    expect(filterMessages([m], 'broadcast')).toEqual([]);
  });

  it('files an outgoing DM under the destination peer bucket', () => {
    const m = msg({ direction: 'outgoing', from: SELF, to: PEER });
    expect(filterMessages([m], `dm:${PEER}`)).toEqual([m]);
  });
});
