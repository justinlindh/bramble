import { describe, expect, it, beforeEach, afterEach, vi } from 'vitest';
import { fireEvent, render, screen, waitFor } from '@testing-library/react';
import { TrafficMonitor } from './TrafficMonitor';

const mockLoadTrafficDebugStatus = vi.fn();
const mockLoadTrafficEvents = vi.fn();

const storeState: any = {
  connectionState: 'connected',
  trafficDebugStatus: {
    config: { enabled: true, includeTx: true, includeRx: true, sampleRate: 100 },
    ringSize: 256,
    ringUsed: 0,
    droppedCount: 0,
    lastSeq: 0,
  },
  trafficEvents: [],
};

vi.mock('../../store/index', () => ({
  useStore: (selector: any) => selector(storeState),
}));

vi.mock('../../store/actions', () => ({
  loadTrafficDebugStatus: () => mockLoadTrafficDebugStatus(),
  loadTrafficEvents: (highestSeq?: number) => mockLoadTrafficEvents(highestSeq),
}));

describe('TrafficMonitor filter persistence', () => {
  beforeEach(() => {
    localStorage.clear();
    mockLoadTrafficDebugStatus.mockReset();
    mockLoadTrafficEvents.mockReset();
  });

  it('restores persisted filters on mount', () => {
    localStorage.setItem(
      'bramble:traffic-monitor:filters',
      JSON.stringify({ category: 'routing', direction: 'rx', bucket: 'critical' }),
    );

    render(<TrafficMonitor />);

    expect((screen.getByRole('combobox', { name: 'Category' }) as HTMLSelectElement).value).toBe('routing');
    expect((screen.getByRole('combobox', { name: 'Direction' }) as HTMLSelectElement).value).toBe('rx');
    expect((screen.getByRole('combobox', { name: 'Bucket' }) as HTMLSelectElement).value).toBe('critical');
  });

  it('persists filter changes and reuses them after remount', async () => {
    const { unmount } = render(<TrafficMonitor />);

    fireEvent.change(screen.getByRole('combobox', { name: 'Category' }), { target: { value: 'beacon' } });
    fireEvent.change(screen.getByRole('combobox', { name: 'Direction' }), { target: { value: 'tx' } });
    fireEvent.change(screen.getByRole('combobox', { name: 'Bucket' }), { target: { value: 'broadcast' } });

    await waitFor(() => {
      expect(localStorage.getItem('bramble:traffic-monitor:filters')).toBe(
        JSON.stringify({ category: 'beacon', direction: 'tx', bucket: 'broadcast' }),
      );
    });

    unmount();
    render(<TrafficMonitor />);

    expect((screen.getByRole('combobox', { name: 'Category' }) as HTMLSelectElement).value).toBe('beacon');
    expect((screen.getByRole('combobox', { name: 'Direction' }) as HTMLSelectElement).value).toBe('tx');
    expect((screen.getByRole('combobox', { name: 'Bucket' }) as HTMLSelectElement).value).toBe('broadcast');
  });

  it('falls back to sane defaults when persisted storage is corrupt', () => {
    localStorage.setItem('bramble:traffic-monitor:filters', '{nope');

    render(<TrafficMonitor />);

    expect((screen.getByRole('combobox', { name: 'Category' }) as HTMLSelectElement).value).toBe('all');
    expect((screen.getByRole('combobox', { name: 'Direction' }) as HTMLSelectElement).value).toBe('all');
    expect((screen.getByRole('combobox', { name: 'Bucket' }) as HTMLSelectElement).value).toBe('all');
  });
});


describe('TrafficMonitor polling', () => {
  beforeEach(() => {
    localStorage.clear();
    mockLoadTrafficDebugStatus.mockReset();
    mockLoadTrafficEvents.mockReset();
    storeState.connectionState = 'connected';
    storeState.trafficEvents = [];
    vi.useFakeTimers();
  });

  afterEach(() => {
    vi.useRealTimers();
  });

  it('polls debug status every ~5s and traffic events every ~2s with incremental since_seq', async () => {
    const { rerender } = render(<TrafficMonitor />);

    expect(mockLoadTrafficDebugStatus).toHaveBeenCalledTimes(1);
    expect(mockLoadTrafficEvents).toHaveBeenCalledTimes(1);
    expect(mockLoadTrafficEvents).toHaveBeenLastCalledWith(undefined);

    storeState.trafficEvents = [
      {
        seq: 42,
        timestampMs: Date.now(),
        direction: 'rx',
        category: 'routing',
        packetType: 'Routing',
        tier: 'normal',
        airtimeBucket: 'normal',
        airtimeDebitUs: 1000,
      } as any,
    ];
    rerender(<TrafficMonitor />);

    await vi.advanceTimersByTimeAsync(2000);
    expect(mockLoadTrafficEvents).toHaveBeenCalledTimes(2);
    expect(mockLoadTrafficEvents).toHaveBeenLastCalledWith(42);

    await vi.advanceTimersByTimeAsync(3000);
    expect(mockLoadTrafficDebugStatus).toHaveBeenCalledTimes(2);
  });

  it('no-ops polling while disconnected', async () => {
    storeState.connectionState = 'disconnected';
    render(<TrafficMonitor />);

    await vi.advanceTimersByTimeAsync(6000);

    expect(mockLoadTrafficDebugStatus).not.toHaveBeenCalled();
    expect(mockLoadTrafficEvents).not.toHaveBeenCalled();
  });
});

describe('TrafficMonitor loading vs disabled state', () => {
  const originalTrafficDebugStatus = storeState.trafficDebugStatus;

  beforeEach(() => {
    localStorage.clear();
    mockLoadTrafficDebugStatus.mockReset();
    mockLoadTrafficEvents.mockReset();
    storeState.connectionState = 'connected';
    storeState.trafficEvents = [];
  });

  afterEach(() => {
    storeState.trafficDebugStatus = originalTrafficDebugStatus;
  });

  it('renders a loading affordance when trafficDebugStatus has never been fetched since connect', () => {
    storeState.trafficDebugStatus = null;
    render(<TrafficMonitor />);

    expect(screen.getByText('Loading traffic monitor…')).toBeInTheDocument();
    expect(screen.queryByText(/Traffic debug is disabled/)).not.toBeInTheDocument();
  });

  it('renders the disabled message once status has been fetched and debug is off', () => {
    storeState.trafficDebugStatus = {
      config: { enabled: false, includeTx: true, includeRx: true, sampleRate: 100 },
      ringSize: 256,
      ringUsed: 0,
      droppedCount: 0,
      lastSeq: 0,
    };
    render(<TrafficMonitor />);

    expect(screen.queryByText('Loading traffic monitor…')).not.toBeInTheDocument();
    expect(screen.getByText('Traffic debug is disabled. Enable it in Config to view monitor data.')).toBeInTheDocument();
  });
});
