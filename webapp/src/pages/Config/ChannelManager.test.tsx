import { afterEach, describe, expect, it, vi } from 'vitest';
import { cleanup, fireEvent, render, screen } from '@testing-library/react';
import type { Channel } from '../../types/bramble';

vi.mock('../../store/actions', () => ({
  addChannel: vi.fn(),
  removeChannel: vi.fn(),
  setDefaultChannel: vi.fn(),
}));

import { ChannelManager } from './ChannelManager';

afterEach(cleanup);

const PSK_CHANNEL: Channel = { index: 1, name: 'secret', hasPsk: true, epoch: 0, isDefault: false };
const OPEN_CHANNEL: Channel = { index: 2, name: 'open', hasPsk: false, epoch: 0, isDefault: false };

describe('ChannelManager PSK prompt dialog', () => {
  it('exposes dialog role/aria-modal and closes on Escape', () => {
    render(<ChannelManager channels={[PSK_CHANNEL]} />);
    fireEvent.click(screen.getByTitle('Share channel "secret" as QR code'));

    const dialog = screen.getByRole('dialog', { name: 'Share "secret"' });
    expect(dialog).toHaveAttribute('aria-modal', 'true');

    fireEvent.keyDown(dialog, { key: 'Escape' });
    expect(screen.queryByRole('dialog', { name: 'Share "secret"' })).not.toBeInTheDocument();
  });

  it('still closes on backdrop click and via the close button', () => {
    render(<ChannelManager channels={[PSK_CHANNEL]} />);
    fireEvent.click(screen.getByTitle('Share channel "secret" as QR code'));
    fireEvent.click(screen.getByRole('dialog', { name: 'Share "secret"' }));
    expect(screen.queryByRole('dialog', { name: 'Share "secret"' })).not.toBeInTheDocument();

    fireEvent.click(screen.getByTitle('Share channel "secret" as QR code'));
    fireEvent.click(screen.getByLabelText('Close'));
    expect(screen.queryByRole('dialog', { name: 'Share "secret"' })).not.toBeInTheDocument();
  });
});

describe('ChannelManager confirm-action dialog', () => {
  it('exposes dialog role/aria-modal and closes on Escape', () => {
    render(<ChannelManager channels={[OPEN_CHANNEL]} />);
    fireEvent.click(screen.getByTitle('Set as default channel'));

    const dialog = screen.getByRole('dialog', { name: 'Set "open" as default channel?' });
    expect(dialog).toHaveAttribute('aria-modal', 'true');

    fireEvent.keyDown(dialog, { key: 'Escape' });
    expect(screen.queryByRole('dialog', { name: 'Set "open" as default channel?' })).not.toBeInTheDocument();
  });

  it('still closes on backdrop click and via Cancel', () => {
    render(<ChannelManager channels={[OPEN_CHANNEL]} />);

    fireEvent.click(screen.getByTitle('Set as default channel'));
    fireEvent.click(screen.getByRole('dialog', { name: 'Set "open" as default channel?' }));
    expect(screen.queryByRole('dialog', { name: 'Set "open" as default channel?' })).not.toBeInTheDocument();

    fireEvent.click(screen.getByTitle('Set as default channel'));
    fireEvent.click(screen.getByText('Cancel'));
    expect(screen.queryByRole('dialog', { name: 'Set "open" as default channel?' })).not.toBeInTheDocument();
  });
});
