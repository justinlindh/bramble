import { describe, it, expect } from 'vitest';
import { buildChannelItems, filterDmConversations } from '../../src/pages/Chat/ConversationList';

describe('ConversationList logic', () => {
  it('builds channels from config and merges unread counts from conversations', () => {
    const config = { channels: [{ index: 0, name: 'Public' }, { index: 1, name: 'Ops' }, { index: 2, name: '' }] } as any;
    const conversations = new Map<string, any>([
      ['ch:1', { id: 'ch:1', unreadCount: 3 }],
      ['ch:2', { id: 'ch:2', unreadCount: 1 }],
    ]);

    const channels = buildChannelItems(config, conversations);
    expect(channels).toEqual([
      { id: 'ch:1', label: 'Ops', unreadCount: 3, hasPsk: false },
      { id: 'ch:2', label: 'ch-2', unreadCount: 1, hasPsk: false },
    ]);
  });

  it('filters DM conversations and excludes broadcast-address DM entries', () => {
    const conversations = new Map<string, any>([
      ['dm:1234', { id: 'dm:1234' }],
      ['dm:4294967295', { id: 'dm:4294967295' }],
      ['ch:1', { id: 'ch:1' }],
    ]);

    const dms = filterDmConversations(conversations as any);
    expect(dms.map(d => d.id)).toEqual(['dm:1234']);
  });
});
