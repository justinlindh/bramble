import { describe, expect, it, vi } from 'vitest';
import { render, screen } from '@testing-library/react';
import { ChannelDetailPanel } from './ChannelDetailPanel';

vi.mock('../../store/actions', () => ({
  removeChannel: vi.fn(),
}));

vi.mock('../../store/index', () => ({
  useStore: (selector: any) => selector({
    config: {
      channels: [
        { index: 1, name: 'ops', hasPsk: true, epoch: 4, isDefault: false },
      ],
    },
    setActiveConversation: vi.fn(),
  }),
}));

describe('ChannelDetailPanel', () => {
  it('shows key epoch help text describing decryption mismatch behavior', () => {
    render(<ChannelDetailPanel channelIndex={1} onClose={() => {}} />);

    const help = screen.getByLabelText('Key epoch info');
    expect(help).toHaveAttribute('title', expect.stringContaining('version counter'));
    expect(help).toHaveAttribute('title', expect.stringContaining('same epoch to decrypt messages'));
    expect(help).toHaveAttribute('title', expect.stringContaining('missed a key rotation'));
  });
});
