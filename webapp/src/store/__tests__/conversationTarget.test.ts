import { beforeEach, describe, expect, it } from 'vitest';
import { useStore, __conversationTargetForMessage as targetFor, conversationIdForMessage } from '../index';
import { normalizeIncomingRealtimeMessage } from '../actions';
import type { Message } from '../../types/bramble';

const SELF = 0x11111111;
const PEER = 0xa1b2c3d4;

function msg(overrides: Partial<Message>): Message {
  return {
    id: `m-${Math.random()}`,
    direction: 'incoming',
    from: PEER,
    to: SELF,
    text: 'hello',
    timestampMs: 1_000,
    status: 'delivered',
    ...overrides,
  } as Message;
}

function resetStore() {
  useStore.setState({ messages: [], conversations: new Map(), activeConversationId: 'broadcast' });
}

describe('conversation target resolution', () => {
  beforeEach(resetStore);

  // The invariant: whatever decides the bucket id also decides peerAddr and
  // channelIndex, so the three can never describe different kinds of message.
  const cases: Array<{ name: string; m: Message; id: string; peerAddr: number | undefined; channelIndex: number | undefined }> = [
    {
      name: 'incoming DM (no channel index)',
      m: msg({ direction: 'incoming', from: PEER, to: SELF }),
      id: `dm:${PEER}`,
      peerAddr: PEER,
      channelIndex: undefined,
    },
    {
      name: 'outgoing DM keys off the destination',
      m: msg({ direction: 'outgoing', from: SELF, to: PEER }),
      id: `dm:${PEER}`,
      peerAddr: PEER,
      channelIndex: undefined,
    },
    {
      name: 'broadcast',
      m: msg({ direction: 'incoming', from: PEER, to: 0xffffffff }),
      id: 'broadcast',
      peerAddr: undefined,
      channelIndex: undefined,
    },
    {
      name: 'channel message',
      m: msg({ direction: 'incoming', from: PEER, to: SELF, channelIndex: 2 }),
      id: 'ch:2',
      peerAddr: undefined,
      channelIndex: 2,
    },
    {
      name: 'channel 0 is a real channel, not a falsy sentinel',
      m: msg({ direction: 'incoming', from: PEER, to: SELF, channelIndex: 0 }),
      id: 'ch:0',
      peerAddr: undefined,
      channelIndex: 0,
    },
    // The firmware sentinel for "not a channel message" is a negative channel
    // index (MSG_STORE_DM_CHANNEL === -1). Both decode paths normalize it away
    // before a Message reaches the store, so this is a backstop: if one ever
    // slips through it must behave as a DM in every field, not land in the
    // right conversation with no peer attached (issue #153).
    {
      name: 'DM carrying the -1 sentinel resolves as a DM, peer included',
      m: msg({ direction: 'incoming', from: PEER, to: SELF, channelIndex: -1 }),
      id: `dm:${PEER}`,
      peerAddr: PEER,
      channelIndex: undefined,
    },
    {
      name: 'broadcast carrying the -1 sentinel resolves as a broadcast',
      m: msg({ direction: 'incoming', from: PEER, to: 0xffffffff, channelIndex: -1 }),
      id: 'broadcast',
      peerAddr: undefined,
      channelIndex: undefined,
    },
  ];

  for (const c of cases) {
    it(`resolves ${c.name}`, () => {
      expect(targetFor(c.m)).toEqual({ id: c.id, peerAddr: c.peerAddr, channelIndex: c.channelIndex });
    });

    it(`addMessage files ${c.name} with agreeing peerAddr and channelIndex`, () => {
      resetStore();
      useStore.getState().addMessage(c.m);
      const conv = useStore.getState().conversations.get(c.id);
      expect(conv).toBeDefined();
      expect(conv!.peerAddr).toBe(c.peerAddr);
      expect(conv!.channelIndex).toBe(c.channelIndex);
    });

    it(`loadCachedMessages files ${c.name} with agreeing peerAddr and channelIndex`, () => {
      resetStore();
      useStore.getState().loadCachedMessages([c.m]);
      const conv = useStore.getState().conversations.get(c.id);
      expect(conv).toBeDefined();
      expect(conv!.peerAddr).toBe(c.peerAddr);
      expect(conv!.channelIndex).toBe(c.channelIndex);
    });

    // The delivery-event correlation and notification paths in store/actions.ts
    // only need the bucket id, but they must derive it through the same single
    // classifier as the store. This pins that agreement for ${c.name}; a fourth
    // hand-rolled copy of the ordering (issue #189) would break exactly here.
    it(`conversationIdForMessage agrees with the store bucket for ${c.name}`, () => {
      expect(conversationIdForMessage(c.m)).toBe(c.id);
      expect(conversationIdForMessage(c.m)).toBe(targetFor(c.m).id);

      resetStore();
      useStore.getState().addMessage(c.m);
      expect(useStore.getState().conversations.has(conversationIdForMessage(c.m))).toBe(true);
    });
  }

  // Guards against the specific drift in #153: a DM bucket must always carry a
  // peer, and a channel bucket must always carry a non-negative index, no
  // matter what channelIndex the message arrived with.
  it('never produces a DM bucket without a peer address', () => {
    for (const channelIndex of [undefined, -1, -5]) {
      const t = targetFor(msg({ direction: 'incoming', from: PEER, to: SELF, channelIndex }));
      expect(t.id).toBe(`dm:${PEER}`);
      expect(t.peerAddr).toBe(PEER);
    }
  });

  it('never produces a channel bucket with a negative index or a peer address', () => {
    for (const channelIndex of [undefined, -1, 0, 3]) {
      const t = targetFor(msg({ direction: 'incoming', from: PEER, to: SELF, channelIndex }));
      if (t.id.startsWith('ch:')) {
        expect(t.channelIndex).toBeGreaterThanOrEqual(0);
        expect(t.peerAddr).toBeUndefined();
      } else {
        expect(t.channelIndex).toBeUndefined();
      }
    }
  });
});

// The store-side >= 0 test is a backstop precisely because these decode paths
// strip the sentinel first. If that stops being true, the backstop above is the
// only thing standing between a -1 and a 'ch:-1' bucket, so pin it here.
describe('RPC decode strips the firmware -1 channel sentinel', () => {
  it('normalizes channel -1 to undefined on realtime messages', () => {
    const m = normalizeIncomingRealtimeMessage({ from: 'A1B2C3D4', to: '11111111', text: 'hi', channel: -1 });
    expect(m.channelIndex).toBeUndefined();
  });

  it('keeps a non-negative channel index', () => {
    const m = normalizeIncomingRealtimeMessage({ from: 'A1B2C3D4', to: '11111111', text: 'hi', channel: 0 });
    expect(m.channelIndex).toBe(0);
  });
});
