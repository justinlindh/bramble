import { describe, it, expect, beforeEach } from 'vitest';
import { render, screen } from '@testing-library/react';
import { NeighborCard } from '../../src/pages/Nodes/NeighborCard';
import { useStore } from '../../src/store';
import type { Neighbor, PeerLocation } from '../../src/types/bramble';

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

function makePeerLocation(lastUpdatedMs: number): PeerLocation {
  return {
    addr: 0x1234abcd,
    name: 'Test Peer',
    tier: 'coarse',
    position: null,
    gridSquare: 'FN31',
    online: true,
    lastUpdatedMs,
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

  it('shows location action when a peer location exists, even if older than freshness threshold', () => {
    const staleLocation = makePeerLocation(Date.now() - (31 * 60 * 1000));

    render(<NeighborCard neighbor={makeNeighbor(5_000)} peerLocation={staleLocation} />);

    expect(screen.getByText(/show location:/i, { selector: 'button' })).toBeInTheDocument();
    expect(screen.queryByText(/location: unavailable/i)).not.toBeInTheDocument();
  });
});
