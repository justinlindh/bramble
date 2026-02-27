import { describe, it, expect, beforeEach } from 'vitest';
import { render, screen } from '@testing-library/react';
import { NeighborCard } from '../../src/pages/Nodes/NeighborCard';
import { useStore } from '../../src/store';
import type { Neighbor } from '../../src/types/bramble';

function makeNeighbor(lastHeardMs: number): Neighbor {
  return {
    addr: 0x1234abcd,
    rssi: -82,
    snr: 8.2,
    deliveryRate: 240,
    lastHeardMs,
    airtimeRemaining: 93,
  };
}

describe('NeighborCard staleness', () => {
  beforeEach(() => {
    useStore.setState({ peerNames: new Map() });
  });

  it('shows stale visual cue when lastHeardMs is greater than 10 minutes', () => {
    const { container } = render(<NeighborCard neighbor={makeNeighbor(10 * 60 * 1000 + 1)} />);

    expect(screen.getByText('Stale')).toBeInTheDocument();
    expect(container.querySelector('article')?.className).toContain('staleCard');
    expect(screen.queryByText('Active')).not.toBeInTheDocument();
  });

  it('keeps neighbor as active at the exact 10 minute threshold', () => {
    const { container } = render(<NeighborCard neighbor={makeNeighbor(10 * 60 * 1000)} />);

    expect(screen.getByText('Active')).toBeInTheDocument();
    expect(screen.queryByText('Stale')).not.toBeInTheDocument();
    expect(container.querySelector('article')?.className).not.toContain('staleCard');
  });
});
