import { beforeEach, describe, expect, it, vi } from 'vitest';
import { fireEvent, render, screen } from '@testing-library/react';
import { ChannelManager } from '../../src/pages/Config/ChannelManager';
import { removeChannel, setDefaultChannel } from '../../src/store/actions';

vi.mock('../../src/store/actions', () => ({
  addChannel: vi.fn(),
  removeChannel: vi.fn(),
  setDefaultChannel: vi.fn(),
}));

vi.mock('../../src/components/QRShareModal', () => ({
  QRShareModal: () => null,
}));

vi.mock('../../src/components/QRScanModal', () => ({
  QRScanModal: () => null,
}));

describe('ChannelManager confirmations', () => {
  const channels = [
    { index: 1, name: 'ops', hasPsk: false, epoch: 1, isDefault: false },
    { index: 2, name: 'public', hasPsk: false, epoch: 1, isDefault: true },
  ] as any;

  beforeEach(() => {
    vi.clearAllMocks();
  });

  it('does not set default when confirmation is cancelled', () => {
    render(<ChannelManager channels={channels} />);
    fireEvent.click(screen.getByRole('button', { name: /set default/i }));

    // In-app confirmation dialog should appear
    expect(screen.getByText(/Set "ops" as default channel\?/)).toBeTruthy();

    // Click Cancel
    fireEvent.click(screen.getByText('Cancel'));

    expect(setDefaultChannel).not.toHaveBeenCalled();
    // Dialog should be dismissed
    expect(screen.queryByText(/Set "ops" as default channel\?/)).toBeNull();
  });

  it('sets default when confirmation is accepted', () => {
    render(<ChannelManager channels={channels} />);
    fireEvent.click(screen.getByRole('button', { name: /set default/i }));

    expect(screen.getByText(/Set "ops" as default channel\?/)).toBeTruthy();

    // Click Confirm
    fireEvent.click(screen.getByText('Confirm'));

    expect(setDefaultChannel).toHaveBeenCalledWith(1);
  });

  it('does not remove channel when confirmation is cancelled', () => {
    render(<ChannelManager channels={channels} />);
    fireEvent.click(screen.getByTitle('Remove channel ops'));

    expect(screen.getByText(/Remove channel "ops"\?/)).toBeTruthy();

    fireEvent.click(screen.getByText('Cancel'));

    expect(removeChannel).not.toHaveBeenCalled();
    expect(screen.queryByText(/Remove channel "ops"\?/)).toBeNull();
  });

  it('removes channel when confirmation is accepted', () => {
    render(<ChannelManager channels={channels} />);
    fireEvent.click(screen.getByTitle('Remove channel ops'));

    expect(screen.getByText(/Remove channel "ops"\?/)).toBeTruthy();

    fireEvent.click(screen.getByText('Confirm'));

    expect(removeChannel).toHaveBeenCalledWith(1);
  });
});
