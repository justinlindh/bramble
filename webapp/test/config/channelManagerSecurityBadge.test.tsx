import { describe, expect, it, vi } from 'vitest';
import { render, screen } from '@testing-library/react';
import { ChannelManager } from '../../src/pages/Config/ChannelManager';

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

describe('ChannelManager PSK badge visibility', () => {
  it('shows PSK badge only for protected channels', () => {
    render(
      <ChannelManager
        channels={[
          { index: 1, name: 'ops', hasPsk: true, epoch: 1, isDefault: false },
          { index: 2, name: 'public', hasPsk: false, epoch: 1, isDefault: false },
        ] as any}
      />
    );

    expect(screen.getByText('ops')).toBeInTheDocument();
    expect(screen.getByText('public')).toBeInTheDocument();
    expect(screen.getAllByText('PSK')).toHaveLength(1);
  });
});
