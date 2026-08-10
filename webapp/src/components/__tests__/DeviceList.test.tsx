import { act, render, screen, fireEvent, cleanup, waitFor } from '@testing-library/react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { DeviceList } from '../DeviceList';
import { useStore } from '../../store/index';
import { upsertDevice, listDevices } from '../../lib/deviceBook';
import * as actions from '../../store/actions';
import type { SavedDevice } from '../../lib/deviceBook';

// DeviceList is presentational: connecting is the parent's job (via onConnect),
// so these tests cover the row rendering, the in-flight lockout, and the
// rename/forget affordances that live inside the list itself.

function seed(overrides: Partial<Parameters<typeof upsertDevice>[0]> = {}): void {
  upsertDevice({ address: 'DEADBEEF', name: 'V4', lastIp: '198.51.100.146', transport: 'wifi', remember: true, nowMs: 1, ...overrides });
  actions.refreshDevices();
}

function renderList(props: Partial<{ onConnect: (d: SavedDevice) => void; busyAddress: string | null; disabled: boolean }> = {}) {
  return render(
    <DeviceList
      onConnect={props.onConnect ?? (() => {})}
      busyAddress={props.busyAddress ?? null}
      disabled={props.disabled ?? false}
    />,
  );
}

beforeEach(() => {
  localStorage.clear(); sessionStorage.clear();
  useStore.getState().setDevices([]);
});
afterEach(cleanup);

describe('DeviceList', () => {
  it('renders saved devices and clicking a row hands the device to onConnect', () => {
    seed();
    const onConnect = vi.fn();
    renderList({ onConnect });
    expect(screen.getByText('V4')).toBeInTheDocument();
    fireEvent.click(screen.getByRole('button', { name: /connect to V4/i }));
    expect(onConnect).toHaveBeenCalledWith(expect.objectContaining({ address: 'DEADBEEF', transport: 'wifi' }));
  });

  it('marks the busy row aria-busy with a spinner and locks every control while disabled', () => {
    seed();
    seed({ address: 'CAFEBABE', name: 'Shed', nowMs: 2 });
    renderList({ busyAddress: 'DEADBEEF', disabled: true });
    const busyRow = screen.getByRole('button', { name: /connect to V4/i });
    expect(busyRow).toHaveAttribute('aria-busy', 'true');
    expect(busyRow.querySelector('[class*="spinner"]')).not.toBeNull();
    const idleRow = screen.getByRole('button', { name: /connect to Shed/i });
    expect(idleRow).toHaveAttribute('aria-busy', 'false');
    // A second click mid-connect tears down the first attempt and overlaps
    // pairing sequences, so EVERYTHING locks: rows, rename, and forget.
    for (const btn of screen.getAllByRole('button')) {
      expect(btn).toBeDisabled();
    }
  });

  it('rename can be cancelled with the Cancel button', () => {
    seed();
    renderList();
    fireEvent.click(screen.getByRole('button', { name: /rename V4/i }));
    fireEvent.click(screen.getByRole('button', { name: /cancel/i }));
    expect(screen.queryByLabelText(/rename V4/i, { selector: 'input' })).toBeNull();
    expect(listDevices()[0].name).toBe('V4');
  });

  it('rename can be cancelled with Escape', () => {
    seed();
    renderList();
    fireEvent.click(screen.getByRole('button', { name: /rename V4/i }));
    const input = screen.getByRole('textbox', { name: /rename V4/i });
    fireEvent.change(input, { target: { value: 'Renamed' } });
    fireEvent.keyDown(input, { key: 'Escape' });
    expect(screen.queryByRole('textbox', { name: /rename V4/i })).toBeNull();
    expect(listDevices()[0].name).toBe('V4'); // draft discarded
  });

  it('closing rename returns focus to the row\'s Rename button', async () => {
    // The rename form unmounts the focused input; without restoration a
    // keyboard user's position falls to <body> after every rename.
    seed();
    renderList();
    fireEvent.click(screen.getByRole('button', { name: /rename V4/i }));
    const input = screen.getByRole('textbox', { name: /rename V4/i });
    fireEvent.keyDown(input, { key: 'Escape' });
    await waitFor(() => expect(screen.getByRole('button', { name: /rename V4/i })).toHaveFocus());
  });

  it('Escape on the rename Cancel button also cancels', () => {
    seed();
    renderList();
    fireEvent.click(screen.getByRole('button', { name: /rename V4/i }));
    fireEvent.keyDown(screen.getByRole('button', { name: /cancel/i }), { key: 'Escape' });
    expect(screen.queryByRole('textbox', { name: /rename V4/i })).toBeNull();
  });

  it('a confirmed forget moves focus to the list heading when devices remain', async () => {
    // The focused Forget button vanishes with its row; land somewhere
    // deterministic instead of <body>.
    seed();
    upsertDevice({ address: 'AAAA0002', name: 'Second', lastIp: '192.0.2.7', transport: 'wifi', remember: false, nowMs: 2 });
    actions.refreshDevices();
    renderList();
    fireEvent.click(screen.getByRole('button', { name: /forget V4/i }));
    fireEvent.click(screen.getByRole('button', { name: /confirm forget V4/i }));
    await waitFor(() => expect(screen.getByRole('heading', { name: /your devices/i })).toHaveFocus());
  });

  it('forget is a two-step confirm: first click arms, second click executes', () => {
    seed();
    renderList();
    fireEvent.click(screen.getByRole('button', { name: /forget V4/i }));
    // Still present after the first click; the button now asks for confirmation.
    expect(screen.getByText('V4')).toBeInTheDocument();
    const confirm = screen.getByRole('button', { name: /confirm forget V4/i });
    fireEvent.click(confirm);
    expect(screen.queryByText('V4')).not.toBeInTheDocument();
    expect(listDevices()).toHaveLength(0);
  });

  it('an armed forget disarms on its own after ~4s', () => {
    vi.useFakeTimers();
    try {
      seed();
      renderList();
      fireEvent.click(screen.getByRole('button', { name: /forget V4/i }));
      expect(screen.getByRole('button', { name: /confirm forget V4/i })).toBeInTheDocument();
      act(() => { vi.advanceTimersByTime(4500); });
      expect(screen.queryByRole('button', { name: /confirm forget V4/i })).toBeNull();
      expect(listDevices()).toHaveLength(1); // nothing was deleted
    } finally {
      vi.useRealTimers();
    }
  });
});
