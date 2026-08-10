import { describe, expect, it, vi } from 'vitest';
import { fireEvent, render, screen } from '@testing-library/react';
import type { Conversation } from '../../types/bramble';
import { ConversationList, buildChannelItems, filterDmConversations } from './ConversationList';

vi.mock('../../store/actions', () => ({
  addChannel: vi.fn(),
}));

// Keep the real module (parseConversationId and friends) and only stub the
// store hook.
vi.mock('../../store/index', async (importOriginal) => ({
  ...(await importOriginal<typeof import('../../store/index')>()),
  useStore: (selector: any) => selector({
    config: {
      channels: [
        { index: 1, name: 'alpha', hasPsk: true },
        { index: 2, name: 'beta', hasPsk: false },
      ],
    },
    neighbors: [],
    routes: [],
    peerLocations: [],
  }),
}));

describe('buildChannelItems', () => {
  it('keeps metadata name when present and does not fallback to ch-{index}', () => {
    const items = buildChannelItems(
      { channels: [{ index: 2, name: '  alpha-team  ', hasPsk: true, epoch: 0, isDefault: false }] },
      new Map<string, Conversation>()
    );

    expect(items).toHaveLength(1);
    expect(items[0].label).toBe('alpha-team');
    expect(items[0].hasPsk).toBe(true);
  });

  it('falls back only when name is missing/blank', () => {
    const items = buildChannelItems(
      { channels: [{ index: 5, name: '   ', hasPsk: false, epoch: 0, isDefault: false }] },
      new Map<string, Conversation>()
    );

    expect(items[0].label).toBe('ch-5');
  });
});

describe('filterDmConversations', () => {
  it('hides stale DMs when known peer set is provided', () => {
    const conversations = new Map<string, Conversation>([
      ['dm:1234', { id: 'dm:1234', label: 'A', peerAddr: 1234, unreadCount: 0 }],
      ['dm:9999', { id: 'dm:9999', label: 'B', peerAddr: 9999, unreadCount: 1 }],
      ['broadcast', { id: 'broadcast', label: 'Broadcast', unreadCount: 0 }],
    ]);

    const filtered = filterDmConversations(conversations, new Set([1234]));

    expect(filtered.map(c => c.id)).toEqual(['dm:1234']);
  });

  it('keeps all DMs when known peer set is empty/unavailable', () => {
    const conversations = new Map<string, Conversation>([
      ['dm:1234', { id: 'dm:1234', label: 'A', peerAddr: 1234, unreadCount: 0 }],
      ['dm:9999', { id: 'dm:9999', label: 'B', peerAddr: 9999, unreadCount: 1 }],
    ]);

    const filtered = filterDmConversations(conversations, new Set());

    expect(filtered.map(c => c.id).sort()).toEqual(['dm:1234', 'dm:9999']);
  });
});

describe('ConversationList channel rendering', () => {
  it('shows lock icon only for PSK-protected channels', () => {
    render(
      <ConversationList
        conversations={new Map<string, Conversation>()}
        activeId="broadcast"
        onSelect={() => {}}
      />
    );

    expect(screen.getByText('alpha')).toBeInTheDocument();
    expect(screen.getByText('beta')).toBeInTheDocument();
    expect(screen.getByLabelText('PSK protected')).toBeInTheDocument();
    expect(screen.getAllByLabelText('PSK protected')).toHaveLength(1);
  });
});

describe('ConversationList dialog a11y', () => {
  it('New Direct Message dialog exposes dialog role/aria-modal and closes on Escape', () => {
    render(
      <ConversationList
        conversations={new Map<string, Conversation>()}
        activeId="broadcast"
        onSelect={() => {}}
      />
    );

    fireEvent.click(screen.getByLabelText('New direct message'));

    const dialog = screen.getByRole('dialog', { name: 'New Direct Message' });
    expect(dialog).toHaveAttribute('aria-modal', 'true');

    fireEvent.keyDown(dialog, { key: 'Escape' });
    expect(screen.queryByRole('dialog', { name: 'New Direct Message' })).not.toBeInTheDocument();
  });

  it('Create Channel dialog exposes dialog role/aria-modal and closes on Escape', () => {
    render(
      <ConversationList
        conversations={new Map<string, Conversation>()}
        activeId="broadcast"
        onSelect={() => {}}
      />
    );

    fireEvent.click(screen.getByLabelText('New channel'));

    const dialog = screen.getByRole('dialog', { name: 'Create Channel' });
    expect(dialog).toHaveAttribute('aria-modal', 'true');

    fireEvent.keyDown(dialog, { key: 'Escape' });
    expect(screen.queryByRole('dialog', { name: 'Create Channel' })).not.toBeInTheDocument();
  });
});
