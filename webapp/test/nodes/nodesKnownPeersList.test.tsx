import { describe, expect, it, vi } from 'vitest';
import { fireEvent, render, screen, within } from '@testing-library/react';
import { Nodes } from '../../src/pages/Nodes/Nodes';

const mockState: any = {
  neighbors: [],
  routes: [],
  peerLocations: [],
  connectionState: 'connected',
  peerNames: new Map<number, string>(),
};

const openDM = vi.fn();
const showOnMap = vi.fn();

vi.mock('../../src/store/index', () => ({
  useStore: (selector: any) => selector(mockState),
}));

vi.mock('../../src/store/actions', () => ({
  loadNeighbors: vi.fn(),
  loadRoutes: vi.fn(),
  loadPeerLocations: vi.fn(),
  openDM: (addr: number) => openDM(addr),
  showOnMap: (addr: number) => showOnMap(addr),
}));

vi.mock('../../src/hooks/usePoll', () => ({
  usePoll: vi.fn(),
}));

vi.mock('../../src/pages/Nodes/RouteTable', () => ({
  RouteTable: () => <div data-testid="route-table" />,
}));

describe('Nodes known peers list', () => {
  it('renders known peers with actions when connected and there are no live neighbors', () => {
    const now = Date.now();
    mockState.neighbors = [];
    mockState.routes = [{ dest: 0x11111111, nextHop: 0x22222222, hopCount: 2, metric: 10, state: 'active', lastUsedMs: 1000 }];
    mockState.peerLocations = [
      {
        addr: 0x11111111,
        name: 'Alpha',
        tier: 'full',
        position: { lat: 1, lon: 2, alt: 0, accuracy: 3, speed: 0, heading: 0, timestampMs: now },
        online: true,
        lastUpdatedMs: now,
      },
      {
        addr: 0x33333333,
        name: 'Bravo',
        tier: 'presence',
        position: null,
        online: false,
        lastUpdatedMs: now,
      },
    ];

    render(<Nodes />);

    expect(screen.getByText('No direct radio neighbors discovered yet.')).toBeInTheDocument();
    expect(screen.getByRole('heading', { name: 'Known peers' })).toBeInTheDocument();
    expect(screen.getByText('Alpha')).toBeInTheDocument();
    expect(screen.getByText('Bravo')).toBeInTheDocument();

    const alphaRow = screen.getByText('Alpha').closest('li');
    expect(alphaRow).toBeTruthy();

    fireEvent.click(within(alphaRow as HTMLElement).getByRole('button', { name: /dm/i }));
    expect(openDM).toHaveBeenCalledWith(0x11111111);

    fireEvent.click(within(alphaRow as HTMLElement).getByRole('button', { name: /show on map/i }));
    expect(showOnMap).toHaveBeenCalledWith(0x11111111);
  });
});
