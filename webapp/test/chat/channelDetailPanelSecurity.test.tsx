import { describe, expect, it, vi } from 'vitest';
import { render, screen } from '@testing-library/react';
import { ChannelDetailPanel } from '../../src/pages/Chat/ChannelDetailPanel';

const mockState: any = {
  config: { channels: [] },
  setActiveConversation: vi.fn(),
};

vi.mock('../../src/store/index', () => ({
  useStore: (selector: any) => selector(mockState),
}));

vi.mock('../../src/store/actions', () => ({
  removeChannel: vi.fn(),
}));

vi.mock('../../src/components/QRShareModal', () => ({
  QRShareModal: () => null,
}));

describe('ChannelDetailPanel security indicator', () => {
  it('shows PSK security text for protected channels', () => {
    mockState.config.channels = [
      { index: 2, name: 'ops-net', hasPsk: true, epoch: 4, isDefault: false },
    ];

    render(<ChannelDetailPanel channelIndex={2} onClose={() => {}} />);

    expect(screen.getByText('# ops-net')).toBeInTheDocument();
    expect(screen.getByText(/Pre-shared key \(PSK\)/)).toBeInTheDocument();
  });

  it('shows open security text for non-PSK channels', () => {
    mockState.config.channels = [
      { index: 3, name: 'public', hasPsk: false, epoch: 1, isDefault: false },
    ];

    render(<ChannelDetailPanel channelIndex={3} onClose={() => {}} />);

    expect(screen.getByText('# public')).toBeInTheDocument();
    expect(screen.getByText('Open (no PSK)')).toBeInTheDocument();
  });
});
