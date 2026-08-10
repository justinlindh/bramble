import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import { cleanup, fireEvent, render, screen } from '@testing-library/react';
import { ConnectionOverlay, connectingLabelFor } from '../ConnectionOverlay';
import { useStore } from '../../store/index';

// First-time BLE pairing raises the OS passkey prompt mid-connect. Fail-fast
// stacks report it via pairingPending (the dynamic banner); Chrome blocks
// silently inside its own dialog, so the static hint copy is the only warning
// path there. Both surfaces are pinned here.

beforeEach(() => {
  localStorage.clear();
  sessionStorage.clear();
  useStore.setState({
    connectionState: 'disconnected',
    connectionError: undefined,
    pairingPending: false,
    devices: [],
  } as any);
});
afterEach(cleanup);

describe('connectingLabelFor', () => {
  it('keeps the per-transport labels when pairing is not pending', () => {
    expect(connectingLabelFor('ble')).toBe('Scanning…');
    expect(connectingLabelFor('serial')).toBe('Opening serial…');
    expect(connectingLabelFor('wifi')).toBe('Handshaking…');
  });

  it('becomes Pairing… while a pairing prompt is pending', () => {
    expect(connectingLabelFor('ble', true)).toBe('Pairing…');
  });
});

describe('ConnectionOverlay pairing surfaces', () => {
  it('shows a pairing banner and Pairing… label while connecting with a prompt up', () => {
    useStore.setState({ connectionState: 'connecting', pairingPending: true } as any);
    render(<ConnectionOverlay />);
    fireEvent.click(screen.getByRole('button', { name: /^bluetooth$/i }));
    const banner = screen.getByRole('alert');
    expect(banner.textContent).toMatch(/pairing code/i);
    expect(banner.textContent).toMatch(/browser prompt/i);
    expect(screen.getByRole('button', { name: /pairing…/i })).toBeInTheDocument();
  });

  it('shows no banner while connecting without a pending prompt', () => {
    useStore.setState({ connectionState: 'connecting', pairingPending: false } as any);
    render(<ConnectionOverlay />);
    expect(screen.queryByRole('alert')).toBeNull();
  });

  it('warns about the pairing code in the static Bluetooth hint (Chrome has no dynamic signal)', () => {
    render(<ConnectionOverlay />);
    fireEvent.click(screen.getByRole('button', { name: /^bluetooth$/i }));
    expect(screen.getByText(/may show a pairing code/i)).toBeInTheDocument();
  });
});
