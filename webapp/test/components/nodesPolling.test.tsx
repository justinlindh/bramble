import { describe, it, expect, vi, beforeEach } from 'vitest';
import { render } from '@testing-library/react';

/**
 * Issue #99: the Nodes tab polled loadNeighbors every 5s while App polled the
 * same loader every 10s and bramble.onNeighborChange already pushed the same
 * refresh. These assertions pin the surviving refresh policy: neighbors are
 * push-driven with a single global safety net (App), routes keep a real poll
 * because no firmware build emits bramble.onRouteUpdate, and every remaining
 * poll is gated on connection state rather than the client-null guard.
 */

let state: any;
vi.mock('../../src/store/index', () => ({
  useStore: (selector: any) => selector(state),
}));

const loadNeighbors = vi.fn(() => Promise.resolve());
const loadRoutes = vi.fn(() => Promise.resolve());
const loadPeerLocations = vi.fn(() => Promise.resolve());

vi.mock('../../src/store/actions', () => ({
  loadNeighbors: (...a: unknown[]) => loadNeighbors(...(a as [])),
  loadRoutes: (...a: unknown[]) => loadRoutes(...(a as [])),
  loadPeerLocations: (...a: unknown[]) => loadPeerLocations(...(a as [])),
  openDM: vi.fn(),
  showOnMap: vi.fn(),
}));

const polls: { fn: () => unknown; interval: number }[] = [];
vi.mock('../../src/hooks/usePoll', () => ({
  usePoll: (fn: () => unknown, interval: number) => { polls.push({ fn, interval }); },
}));

vi.mock('../../src/pages/Nodes/RouteTable', () => ({
  RouteTable: () => <div data-testid="route-table" />,
}));

import { Nodes } from '../../src/pages/Nodes/Nodes';

function baseState(connectionState = 'connected') {
  return {
    neighbors: [],
    routes: [],
    peerLocations: [],
    peerNames: new globalThis.Map<number, string>(),
    connectionState,
  };
}

describe('Nodes refresh policy', () => {
  beforeEach(() => {
    polls.length = 0;
    loadNeighbors.mockClear();
    loadRoutes.mockClear();
    loadPeerLocations.mockClear();
    state = baseState();
  });

  it('registers exactly two polls, both slow safety nets', () => {
    render(<Nodes />);
    expect(polls.map(p => p.interval)).toEqual([30_000, 60_000]);
  });

  it('does not poll neighbors: onNeighborChange plus the global App net covers it', async () => {
    render(<Nodes />);
    // Ignore the one-shot mount refresh: only the registered polls matter here.
    loadNeighbors.mockClear();
    for (const p of polls) await p.fn();
    expect(loadNeighbors).not.toHaveBeenCalled();
    expect(loadRoutes).toHaveBeenCalledTimes(1);
    expect(loadPeerLocations).toHaveBeenCalledTimes(1);
  });

  it('refreshes neighbors once on mount so the tab never opens on stale data', () => {
    render(<Nodes />);
    expect(loadNeighbors).toHaveBeenCalledTimes(1);
  });

  it('gates every poll on connection state', async () => {
    state = baseState('disconnected');
    render(<Nodes />);
    for (const p of polls) await p.fn();
    expect(loadRoutes).not.toHaveBeenCalled();
    expect(loadPeerLocations).not.toHaveBeenCalled();
    expect(loadNeighbors).not.toHaveBeenCalled();
  });
});
