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
    vi.spyOn(window, 'confirm').mockReturnValue(false);

    render(<ChannelManager channels={channels} />);
    fireEvent.click(screen.getByRole('button', { name: /set default/i }));

    expect(window.confirm).toHaveBeenCalledWith('Set default channel "ops"?');
    expect(setDefaultChannel).not.toHaveBeenCalled();
  });

  it('sets default when confirmation is accepted', () => {
    vi.spyOn(window, 'confirm').mockReturnValue(true);

    render(<ChannelManager channels={channels} />);
    fireEvent.click(screen.getByRole('button', { name: /set default/i }));

    expect(window.confirm).toHaveBeenCalledWith('Set default channel "ops"?');
    expect(setDefaultChannel).toHaveBeenCalledWith(1);
  });

  it('does not remove channel when confirmation is cancelled', () => {
    vi.spyOn(window, 'confirm').mockReturnValue(false);

    render(<ChannelManager channels={channels} />);
    fireEvent.click(screen.getByTitle('Remove channel ops'));

    expect(window.confirm).toHaveBeenCalledWith('Remove channel "ops"?');
    expect(removeChannel).not.toHaveBeenCalled();
  });

  it('removes channel when confirmation is accepted', () => {
    vi.spyOn(window, 'confirm').mockReturnValue(true);

    render(<ChannelManager channels={channels} />);
    fireEvent.click(screen.getByTitle('Remove channel ops'));

    expect(window.confirm).toHaveBeenCalledWith('Remove channel "ops"?');
    expect(removeChannel).toHaveBeenCalledWith(1);
  });
});
