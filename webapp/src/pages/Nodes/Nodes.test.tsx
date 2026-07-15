import { describe, it, expect, vi, beforeEach } from 'vitest';
import { render, screen } from '@testing-library/react';

let state: any;
vi.mock('../../store/index', () => ({
  useStore: (selector: any) => selector(state),
}));

vi.mock('../../store/actions', () => ({
  loadNeighbors: vi.fn(),
  loadRoutes: vi.fn(),
  loadPeerLocations: vi.fn(),
  openDM: vi.fn(),
  showOnMap: vi.fn(),
}));

vi.mock('../../hooks/usePoll', () => ({
  usePoll: vi.fn(),
}));

vi.mock('./RouteTable', () => ({
  RouteTable: () => <div data-testid="route-table" />,
}));

import { Nodes } from './Nodes';

describe('Nodes neighbors loading vs empty state', () => {
  beforeEach(() => {
    state = {
      neighbors: undefined,
      routes: [],
      peerLocations: [],
      peerNames: new globalThis.Map<number, string>(),
      connectionState: 'connected',
    };
  });

  it('renders a loading affordance when neighbors have never been fetched since connect', () => {
    render(<Nodes />);
    expect(screen.getByText('Loading neighbors…')).toBeInTheDocument();
    expect(screen.queryByText('No direct radio neighbors discovered yet.')).not.toBeInTheDocument();
  });

  it('renders the loaded-empty message once neighbors have been fetched and there are none', () => {
    state.neighbors = [];
    render(<Nodes />);
    expect(screen.queryByText('Loading neighbors…')).not.toBeInTheDocument();
    expect(screen.getByText('No direct radio neighbors discovered yet.')).toBeInTheDocument();
  });

  it('shows the disconnected hint instead of loading text when never connected', () => {
    state.connectionState = 'disconnected';
    render(<Nodes />);
    expect(screen.queryByText('Loading neighbors…')).not.toBeInTheDocument();
    expect(screen.getByText('Connect to a node to see neighbors.')).toBeInTheDocument();
  });
});
