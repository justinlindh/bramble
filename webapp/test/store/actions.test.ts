import { describe, it, expect, vi, beforeEach } from 'vitest';
import { useStore } from '../../src/store/index';
import type { Message } from '../../src/types/bramble';

function makeMsg(overrides: Partial<Message> = {}): Message {
  return {
    id: crypto.randomUUID(),
    direction: 'outgoing',
    from: 0,
    to: 0x1a3f2b4c,
    text: 'Hello',
    tier: 'normal',
    timestampMs: Date.now(),
    status: 'sending',
    ...overrides,
  };
}

describe('Zustand store — message state transitions', () => {
  beforeEach(() => {
    // Reset store to initial state
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
    });
  });

  it('addMessage creates a conversation entry', () => {
    const msg = makeMsg({ direction: 'outgoing', to: 0x1234, status: 'sending' });
    useStore.getState().addMessage(msg);

    const state = useStore.getState();
    expect(state.messages).toHaveLength(1);
    expect(state.conversations.size).toBe(1);
    expect(state.conversations.has(`dm:${0x1234}`)).toBe(true);
  });

  it('updateMessageStatus: sending → sent', () => {
    const msg = makeMsg({ status: 'sending' });
    useStore.getState().addMessage(msg);
    useStore.getState().updateMessageStatus(msg.id, 'sent');

    const updated = useStore.getState().messages.find(m => m.id === msg.id);
    expect(updated?.status).toBe('sent');
  });

  it('updateMessageStatus: sent → delivered with relay path', () => {
    const msg = makeMsg({ status: 'sent' });
    useStore.getState().addMessage(msg);

    const relayPath = [{ addr: 0x1a3f, rssi: -78 }, { addr: 0x3b7c, rssi: -90 }];
    useStore.getState().updateMessageStatus(msg.id, 'delivered', relayPath);

    const updated = useStore.getState().messages.find(m => m.id === msg.id);
    expect(updated?.status).toBe('delivered');
    expect(updated?.relayPath).toEqual(relayPath);
  });

  it('updateMessageStatus: sent → failed', () => {
    const msg = makeMsg({ status: 'sent' });
    useStore.getState().addMessage(msg);
    useStore.getState().updateMessageStatus(msg.id, 'failed');

    const updated = useStore.getState().messages.find(m => m.id === msg.id);
    expect(updated?.status).toBe('failed');
  });

  it('incoming message increments unread count', () => {
    const msg = makeMsg({
      direction: 'incoming',
      from: 0x5678,
      to: 0,
      status: 'delivered',
    });
    useStore.getState().addMessage(msg);

    const state = useStore.getState();
    const conv = state.conversations.get(`dm:${0x5678}`);
    expect(conv?.unreadCount).toBe(1);
  });

  it('setActiveConversation clears unread count', () => {
    const msg = makeMsg({
      direction: 'incoming',
      from: 0x5678,
      to: 0,
      status: 'delivered',
    });
    useStore.getState().addMessage(msg);
    useStore.getState().addMessage({ ...msg, id: crypto.randomUUID() });

    // Before: 2 unread
    expect(useStore.getState().conversations.get(`dm:${0x5678}`)?.unreadCount).toBe(2);

    useStore.getState().setActiveConversation(`dm:${0x5678}`);

    // After: 0 unread
    expect(useStore.getState().conversations.get(`dm:${0x5678}`)?.unreadCount).toBe(0);
    expect(useStore.getState().activeConversationId).toBe(`dm:${0x5678}`);
  });

  it('incoming message does not increment unread for active conversation', () => {
    // Set conversation as active first
    const convId = `dm:${0x5678}`;
    useStore.getState().setActiveConversation(convId);

    // Add incoming messages while conversation is active
    const msg1 = makeMsg({
      direction: 'incoming',
      from: 0x5678,
      to: 0,
      status: 'delivered',
    });
    useStore.getState().addMessage(msg1);

    // Unread count should remain 0 because conversation is active
    expect(useStore.getState().conversations.get(convId)?.unreadCount).toBe(0);

    // Add another message - still should be 0
    const msg2 = makeMsg({
      ...msg1,
      id: crypto.randomUUID(),
      text: 'Another message',
    });
    useStore.getState().addMessage(msg2);

    expect(useStore.getState().conversations.get(convId)?.unreadCount).toBe(0);
  });

  it('incoming message increments unread for inactive conversation', () => {
    // Set a different conversation as active
    useStore.getState().setActiveConversation('broadcast');

    // Add incoming messages to a different conversation
    const msg = makeMsg({
      direction: 'incoming',
      from: 0x5678,
      to: 0,
      status: 'delivered',
    });
    useStore.getState().addMessage(msg);

    // Unread count should increment because conversation is not active
    const conv = useStore.getState().conversations.get(`dm:${0x5678}`);
    expect(conv?.unreadCount).toBe(1);
  });

  it('caps messages at 500', () => {
    for (let i = 0; i < 510; i++) {
      useStore.getState().addMessage(makeMsg({ id: `msg-${i}` }));
    }
    expect(useStore.getState().messages).toHaveLength(500);
    // The last message should be msg-509, the first msg-10 (oldest 10 dropped)
    expect(useStore.getState().messages[0].id).toBe('msg-10');
    expect(useStore.getState().messages[499].id).toBe('msg-509');
  });

  it('channel messages go to ch: conversation', () => {
    const msg = makeMsg({ channelIndex: 2, direction: 'incoming', from: 0xabc, to: 0 });
    useStore.getState().addMessage(msg);

    const state = useStore.getState();
    expect(state.conversations.has('ch:2')).toBe(true);
  });

  it('setConnectionState stores error message', () => {
    useStore.getState().setConnectionState('error', 'Port not found');
    const state = useStore.getState();
    expect(state.connectionState).toBe('error');
    expect(state.connectionError).toBe('Port not found');
  });

  it('incoming DM creates dm: conversation (not broadcast)', () => {
    // Simulate an incoming DM with channelIndex undefined (as received from firmware)
    const msg = makeMsg({
      direction: 'incoming',
      from: 0x5678,
      to: 0x1234, // specific recipient (not 0xFFFFFFFF)
      channelIndex: undefined,
      status: 'delivered',
    });
    useStore.getState().addMessage(msg);

    const state = useStore.getState();
    // Should create a DM conversation, not broadcast
    expect(state.conversations.has(`dm:${0x5678}`)).toBe(true);
    expect(state.conversations.has('broadcast')).toBe(false);
    
    const conv = state.conversations.get(`dm:${0x5678}`);
    expect(conv?.unreadCount).toBe(1);
    expect(conv?.peerAddr).toBe(0x5678);
  });

  it('incoming DM with channelIndex -1 creates dm: conversation', () => {
    // Some firmware versions may send channelIndex: -1 for DMs
    const msg = makeMsg({
      direction: 'incoming',
      from: 0xABCD,
      to: 0x1234, // specific recipient
      channelIndex: -1, // explicitly -1 (not a channel)
      status: 'delivered',
    });
    useStore.getState().addMessage(msg);

    const state = useStore.getState();
    // Should create a DM conversation, not broadcast
    expect(state.conversations.has(`dm:${0xABCD}`)).toBe(true);
    expect(state.conversations.has('broadcast')).toBe(false);
    
    const conv = state.conversations.get(`dm:${0xABCD}`);
    expect(conv?.unreadCount).toBe(1);
  });

  it('broadcast message creates broadcast conversation', () => {
    const msg = makeMsg({
      direction: 'incoming',
      from: 0x5678,
      to: 0xFFFFFFFF, // broadcast address
      channelIndex: undefined,
      status: 'delivered',
    });
    useStore.getState().addMessage(msg);

    const state = useStore.getState();
    // Should create broadcast conversation, not DM
    expect(state.conversations.has('broadcast')).toBe(true);
    expect(state.conversations.has(`dm:${0x5678}`)).toBe(false);
  });
});
