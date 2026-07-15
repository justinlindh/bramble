import { describe, it, expect, beforeEach } from 'vitest';
import { render, screen, fireEvent } from '@testing-library/react';
import { NeighborCard } from './NeighborCard';
import { useStore } from '../../store';
import type { Neighbor } from '../../types/bramble';

function makeNeighbor(): Neighbor {
  return {
    addr: 0x1234abcd,
    rssi: -82,
    snr: 8.2,
    deliveryRate: 240,
    lastHeardMs: 5_000,
    airtimeRemaining: 93,
  };
}

describe('NeighborCard keyboard activation', () => {
  beforeEach(() => {
    useStore.setState({ peerNames: new Map() });
  });

  it('is a keyboard-focusable button that starts collapsed', () => {
    const { container } = render(<NeighborCard neighbor={makeNeighbor()} />);
    const card = container.querySelector('article') as HTMLElement;
    expect(card).toHaveAttribute('role', 'button');
    expect(card).toHaveAttribute('tabIndex', '0');
    expect(card).toHaveAttribute('aria-expanded', 'false');
  });

  it('toggles expansion on Enter', () => {
    const { container } = render(<NeighborCard neighbor={makeNeighbor()} />);
    const card = container.querySelector('article') as HTMLElement;

    fireEvent.keyDown(card, { key: 'Enter' });
    expect(card).toHaveAttribute('aria-expanded', 'true');
    expect(screen.getByText('Airtime remaining:')).toBeInTheDocument();

    fireEvent.keyDown(card, { key: 'Enter' });
    expect(card).toHaveAttribute('aria-expanded', 'false');
  });

  it('toggles expansion on Space', () => {
    const { container } = render(<NeighborCard neighbor={makeNeighbor()} />);
    const card = container.querySelector('article') as HTMLElement;

    fireEvent.keyDown(card, { key: ' ' });
    expect(card).toHaveAttribute('aria-expanded', 'true');
    expect(screen.getByText('Airtime remaining:')).toBeInTheDocument();
  });

  it('ignores other keys', () => {
    const { container } = render(<NeighborCard neighbor={makeNeighbor()} />);
    const card = container.querySelector('article') as HTMLElement;

    fireEvent.keyDown(card, { key: 'Tab' });
    expect(card).toHaveAttribute('aria-expanded', 'false');
  });
});
