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

function makePeerLocation(online: boolean, lastUpdatedMs: number): PeerLocation {
  return {
    addr: 0x1234abcd,
    name: 'Test Peer',
    tier: 'coarse',
    position: null,
    gridSquare: 'FN31',
    online,
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

  it('shows location action when a peer location exists, even if the node reports it as not current', () => {
    // online:false is how the node reports a position it cannot age, including
    // one carried over from a previous boot. lastUpdatedMs is 0 there, because
    // an earlier boot's uptime reading has no value on the current clock.
    const staleLocation = makePeerLocation(false, 0);

    render(<NeighborCard neighbor={makeNeighbor(5_000)} peerLocation={staleLocation} />);

    expect(screen.getByText(/show location:/i, { selector: 'button' })).toBeInTheDocument();
    expect(screen.queryByText(/location: unavailable/i)).not.toBeInTheDocument();
    expect(
      screen.getByTitle('Show last known location on map'),
    ).toBeInTheDocument();
  });

  it('labels a location the node reports as current', () => {
    render(
      <NeighborCard neighbor={makeNeighbor(5_000)} peerLocation={makePeerLocation(true, 9_000)} />,
    );

    expect(screen.getByTitle('Show location on map')).toBeInTheDocument();
  });
});
