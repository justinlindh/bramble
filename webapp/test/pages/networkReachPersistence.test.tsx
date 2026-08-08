import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { cleanup, render, screen, waitFor } from '@testing-library/react';
import { NetworkReach } from '../../src/pages/Stats/NetworkReach';
import { useStore } from '../../src/store/index';
import type { ProbeResult } from '../../src/types/bramble';

const { sendProbeMock } = vi.hoisted(() => ({
  sendProbeMock: vi.fn(async () => undefined),
}));

vi.mock('../../src/store/actions', () => ({
  sendProbe: sendProbeMock,
}));

const STORAGE_KEY = 'bramble:network-reach:probe-result';

function sampleProbeResult(sentAt = Date.now()): ProbeResult {
  return {
    probeId: 0xabc123,
    sentAt,
    ackWindow: 30,
    complete: true,
    responses: [
      {
        responderAddr: 0x1234,
        hopCount: 1,
        rssi: -88,
        snr: 4.2,
        pathLen: 1,
        seenRounds: 3,
        confidence: 1,
      },
    ],
  };
}

describe('NetworkReach session persistence', () => {
  beforeEach(() => {
    sendProbeMock.mockClear();
    sessionStorage.clear();
    useStore.setState({
      connectionState: 'connected',
      neighbors: [],
      config: { identity: { address: 0x0001 } } as any,
      probeResult: null,
    });
  });

  afterEach(() => {
    cleanup();
    vi.useRealTimers();
  });

  it('persists completed probe results into sessionStorage', async () => {
    useStore.setState({ probeResult: sampleProbeResult() });

    render(<NetworkReach />);

    await waitFor(() => {
      const raw = sessionStorage.getItem(STORAGE_KEY);
      expect(raw).toBeTruthy();
      const parsed = JSON.parse(raw!);
      expect(parsed.probeResult.probeId).toBe(0xabc123);
      expect(typeof parsed.persistedAt).toBe('number');
    });
  });

  it('restores probe results from sessionStorage on mount and shows stale age', async () => {
    const now = Date.now();
    sessionStorage.setItem(
      STORAGE_KEY,
      JSON.stringify({
        probeResult: sampleProbeResult(now - 10 * 60 * 1000),
        persistedAt: now - 7 * 60 * 1000,
      }),
    );

    render(<NetworkReach />);

    expect(await screen.findByText(/Responses: 1 nodes/i)).toBeInTheDocument();
    expect(await screen.findByText(/Results from 7 minutes ago/i)).toBeInTheDocument();
  });

  it('shows a Refresh button after results are available', () => {
    useStore.setState({ probeResult: sampleProbeResult() });

    render(<NetworkReach />);

    const refresh = screen.getByRole('button', { name: /refresh/i });
    expect(refresh).toBeInTheDocument();
  });
});
