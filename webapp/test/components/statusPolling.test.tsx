import { describe, it, expect, vi, beforeEach } from 'vitest';
import { render } from '@testing-library/react';

/**
 * The header GNSS indicator reads bramble.getStatus, which only the Stats tab
 * polled. bramble.onGpsEvent fires on fix transitions, so a node that never
 * acquires a fix (the failure this indicator exists for) emits nothing at all.
 * These assertions pin the global poll that keeps the indicator live on every
 * tab, at an interval chosen to stay well clear of the Stats tab's own 5 s.
 */

let state: any;
vi.mock('../../src/store/index', () => ({
  useStore: Object.assign((selector: any) => selector(state), {
    getState: () => state,
    setState: () => {},
  }),
}));

// Hoisted so the module mock hands App this exact reference: the assertions
// identify the status poll by identity, not by position in the poll list.
const { loadStatus } = vi.hoisted(() => ({ loadStatus: vi.fn(() => Promise.resolve()) }));

vi.mock('../../src/store/actions', () => ({
  disconnect: vi.fn(),
  loadConnectionCapabilities: vi.fn(),
  loadNeighbors: vi.fn(() => Promise.resolve()),
  loadNetworkKeyStatus: vi.fn(() => Promise.resolve()),
  loadStatus,
}));

const polls: { fn: () => unknown; interval: number; enabled: boolean }[] = [];
vi.mock('../../src/hooks/usePoll', () => ({
  usePoll: (fn: () => unknown, interval: number, options?: { enabled?: boolean }) => {
    polls.push({ fn, interval, enabled: options?.enabled ?? true });
  },
}));

vi.mock('../../src/pages/Chat/Chat', () => ({ Chat: () => <div /> }));
vi.mock('../../src/pages/Nodes/Nodes', () => ({ Nodes: () => <div /> }));
vi.mock('../../src/pages/Config/Config', () => ({ Config: () => <div /> }));
vi.mock('../../src/pages/Stats/Stats', () => ({ Stats: () => <div /> }));
vi.mock('../../src/components/ConnectionOverlay', () => ({ ConnectionOverlay: () => null }));
vi.mock('../../src/components/DevicePickerModal', () => ({ DevicePickerModal: () => null }));
vi.mock('../../src/components/UnprovisionedBanner', () => ({ UnprovisionedBanner: () => null }));
vi.mock('../../src/components/GnssDot', () => ({ GnssDot: () => <div data-testid="gnss-dot" /> }));

import App from '../../src/App';

function baseState(connectionState = 'connected') {
  return {
    activeTab: 'chat',
    setActiveTab: vi.fn(),
    connectionState,
    connectionError: null,
    config: null,
    status: null,
  };
}

describe('App status refresh policy', () => {
  beforeEach(() => {
    polls.length = 0;
    loadStatus.mockClear();
    state = baseState();
  });

  it('polls getStatus globally every 15 seconds while connected', () => {
    render(<App />);

    const statusPoll = polls.find(p => p.fn === loadStatus);
    expect(statusPoll).toBeDefined();
    expect(statusPoll!.interval).toBe(15_000);
    expect(statusPoll!.enabled).toBe(true);
  });

  it('gates the poll on the connection rather than issuing work while offline', () => {
    state = baseState('disconnected');
    render(<App />);

    const statusPoll = polls.find(p => p.fn === loadStatus);
    expect(statusPoll).toBeDefined();
    expect(statusPoll!.enabled).toBe(false);
  });

  it('renders the GNSS indicator in the topbar status area when connected', () => {
    const { getByTestId, queryByTestId, unmount } = render(<App />);
    expect(getByTestId('gnss-dot')).toBeInTheDocument();
    unmount();

    state = baseState('disconnected');
    const offline = render(<App />);
    expect(queryByTestId('gnss-dot')).toBeNull();
    offline.unmount();
  });
});
