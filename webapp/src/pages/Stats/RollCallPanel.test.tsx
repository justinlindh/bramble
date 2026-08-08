import { describe, expect, it, vi, beforeEach } from 'vitest';
import { fireEvent, render, screen, waitFor } from '@testing-library/react';
import { RollCallPanel } from './RollCallPanel';
import { useStore } from '../../store/index';
import { loadRollCall, startRollCall } from '../../store/actions';
import type { RollCallLedger } from '../../types/bramble';

vi.mock('../../store/actions', () => ({
  loadRollCall: vi.fn(),
  startRollCall: vi.fn(),
}));

const emptyLedger: RollCallLedger = {
  active: false,
  open: false,
  roundsSent: 0,
  roundsTotal: 3,
  windowMs: 135_000,
  elapsedMs: 0,
  minIntervalMs: 300_000,
  maxTextBytes: 48,
  anchored: false,
  expected: 0,
  responded: 0,
  unattested: 0,
  overflow: 0,
  late: 0,
  pendingDropped: 0,
  missing: [],
  missingCount: 0,
  responders: [],
};

const anchoredLedger: RollCallLedger = {
  ...emptyLedger,
  active: true,
  open: false,
  rollcallId: '0000BEEF',
  text: 'sound off',
  roundsSent: 3,
  elapsedMs: 135_000,
  anchored: true,
  expected: 3,
  responded: 2,
  missing: [0x0000000d],
  missingCount: 1,
  responders: [
    { addr: 0x0000000b, responded: true, atMs: 3200, round: 1, relayPath: [0xaabbccdd, 0x0000000b] },
    { addr: 0x0000000c, responded: true, atMs: 9000, round: 2 },
  ],
};

beforeEach(() => {
  vi.clearAllMocks();
  useStore.setState({ connectionState: 'connected', peerNames: new Map() });
});

describe('RollCallPanel', () => {
  it('shows the anchored ledger with the members that answered and the ones that did not', async () => {
    vi.mocked(loadRollCall).mockResolvedValue(anchoredLedger);

    render(<RollCallPanel />);

    expect(await screen.findByTestId('rollcall-count')).toHaveTextContent('2 of 3 expected');
    expect(screen.getByText('Anchored fleet')).toBeInTheDocument();
    // Both answers are listed with the time INTO the roll-call, not uptime.
    expect(screen.getByText('3.2s in')).toBeInTheDocument();
    expect(screen.getByText('9.0s in')).toBeInTheDocument();
    // The one silent member is named.
    expect(screen.getByTestId('rollcall-missing')).toHaveTextContent('No answer (1)');
    expect(screen.getByTestId('rollcall-missing')).toHaveTextContent('0x000D');
    // The relay path is shown where a receipt supplied one, and honestly
    // marked absent where none did.
    expect(screen.getByText('0xCCDD → 0x000B')).toBeInTheDocument();
    expect(screen.getByText('not reported')).toBeInTheDocument();
  });

  it('says absence proves nothing, next to the ledger that shows it', async () => {
    vi.mocked(loadRollCall).mockResolvedValue(anchoredLedger);

    render(<RollCallPanel />);

    expect(await screen.findByText(/Silence proves nothing on its own/)).toBeInTheDocument();
  });

  it('names nobody missing on an un-anchored node and says why', async () => {
    vi.mocked(loadRollCall).mockResolvedValue({
      ...anchoredLedger,
      anchored: false,
      expected: 0,
      missing: [],
      missingCount: 0,
    });

    render(<RollCallPanel />);

    // No "of N expected" claim can be made without an authoritative set.
    expect(await screen.findByTestId('rollcall-count')).toHaveTextContent('2 responded');
    expect(screen.getByText('Observed only (un-anchored)')).toBeInTheDocument();
    expect(screen.getByText(/cannot name anyone missing/)).toBeInTheDocument();
    expect(screen.queryByTestId('rollcall-missing')).not.toBeInTheDocument();
  });

  it('starts a roll-call with the operator payload and reloads the ledger', async () => {
    vi.mocked(loadRollCall).mockResolvedValue(emptyLedger);
    vi.mocked(startRollCall).mockResolvedValue({
      ok: true,
      rollcallId: '0000BEEF',
      expected: 3,
      anchored: true,
    });

    render(<RollCallPanel />);
    await screen.findByText('No roll call has run on this node yet.');

    fireEvent.change(screen.getByLabelText('Roll-call message'), {
      target: { value: 'sound off' },
    });
    fireEvent.click(screen.getByRole('button', { name: 'Start roll call' }));

    await waitFor(() => expect(startRollCall).toHaveBeenCalledWith('sound off'));
    // The ledger is re-read after starting, so the panel shows the roll-call
    // it just began rather than the stale "none yet" state.
    await waitFor(() => expect(loadRollCall).toHaveBeenCalledTimes(2));
  });

  it('reports a rate-limited refusal with how long to wait, not as an error', async () => {
    vi.mocked(loadRollCall).mockResolvedValue(emptyLedger);
    vi.mocked(startRollCall).mockResolvedValue({
      ok: false,
      reason: 'rate_limited',
      retryAfterMs: 247_000,
      expected: 0,
      anchored: false,
    });

    render(<RollCallPanel />);
    fireEvent.click(await screen.findByRole('button', { name: 'Start roll call' }));

    const notice = await screen.findByRole('status');
    expect(notice).toHaveTextContent('rate limited');
    expect(notice).toHaveTextContent('Try again in 4m 7s.');
    expect(screen.queryByRole('alert')).not.toBeInTheDocument();
  });

  it('disables starting while a roll-call is still collecting', async () => {
    vi.mocked(loadRollCall).mockResolvedValue({
      ...anchoredLedger,
      open: true,
      roundsSent: 1,
      elapsedMs: 30_000,
      responded: 1,
      responders: [anchoredLedger.responders[0]],
      missing: [0x0000000c, 0x0000000d],
      missingCount: 2,
    });

    render(<RollCallPanel />);

    expect(await screen.findByRole('button', { name: 'Start roll call' })).toBeDisabled();
    expect(screen.getByText(/collecting, 1m 45s left \(round 1 of 3\)/)).toBeInTheDocument();
  });
});
