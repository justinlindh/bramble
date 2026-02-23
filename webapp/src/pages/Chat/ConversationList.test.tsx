import { describe, expect, it, vi } from 'vitest';
import { render, screen } from '@testing-library/react';
import type { Conversation } from '../../types/bramble';
import { ConversationList, buildChannelItems } from './ConversationList';

vi.mock('../../store/actions', () => ({
  addChannel: vi.fn(),
}));

vi.mock('../../store/index', () => ({
  useStore: (selector: any) => selector({
    config: {
      channels: [
        { index: 1, name: 'alpha', hasPsk: true },
        { index: 2, name: 'beta', hasPsk: false },
      ],
    },
  }),
}));

describe('buildChannelItems', () => {
  it('keeps metadata name when present and does not fallback to ch-{index}', () => {
    const items = buildChannelItems(
      { channels: [{ index: 2, name: '  alpha-team  ', hasPsk: true }] },
      new Map<string, Conversation>()
    );

    expect(items).toHaveLength(1);
    expect(items[0].label).toBe('alpha-team');
    expect(items[0].hasPsk).toBe(true);
  });

  it('falls back only when name is missing/blank', () => {
    const items = buildChannelItems(
      { channels: [{ index: 5, name: '   ', hasPsk: false }] },
      new Map<string, Conversation>()
    );

    expect(items[0].label).toBe('ch-5');
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
