import { afterEach, beforeEach, describe, expect, it, vi, type Mock } from 'vitest';
import { cleanup, fireEvent, render, screen, waitFor } from '@testing-library/react';
import { ConnectionOverlay } from '../ConnectionOverlay';
import { useStore } from '../../store/index';
import { upsertDevice, setDeviceToken } from '../../lib/deviceBook';
import { refreshDevices } from '../../store/actions';
import { BLETransport } from '../../transport/BLETransport';

// Token hygiene on the BLE device picker: picking a KNOWN device must load
// THAT device's book state over whatever was in the fields, and picking an
// UNKNOWN device must clear them. The old sticky prefill silently carried node
// A's token and name onto node B, which then auth-failed in a way that looked
// like a pairing problem.

vi.mock('../../transport/BLETransport', () => ({
  BLETransport: { pickDevice: vi.fn() },
}));

function seedBook(): void {
  // Most recent BLE device: its token/name seed the blank form on mount.
  upsertDevice({ address: 'AAAA0001', name: 'Porch', transport: 'ble', remember: true, nowMs: 5 });
  setDeviceToken('AAAA0001', 'tokA', true);
  // A second known BLE identity for the known-pick case.
  upsertDevice({ address: 'BBBB0002', name: 'Kitchen', transport: 'ble', remember: true, nowMs: 1, bleDeviceId: 'dev-b' });
  setDeviceToken('BBBB0002', 'tokB', true);
  refreshDevices();
}

async function renderAndPick(picked: { id: string; name?: string }): Promise<void> {
  (BLETransport.pickDevice as Mock).mockResolvedValue(picked);
  render(<ConnectionOverlay />);
  fireEvent.click(screen.getByRole('button', { name: /^bluetooth$/i }));
  // The mount prefill seeds the fields from the most recent BLE device.
  await waitFor(() => expect(screen.getByLabelText('Auth Token')).toHaveValue('tokA'));
  fireEvent.click(screen.getByRole('button', { name: /select device…/i }));
  await waitFor(() => expect(BLETransport.pickDevice).toHaveBeenCalled());
}

beforeEach(() => {
  localStorage.clear();
  sessionStorage.clear();
  useStore.setState({
    connectionState: 'disconnected',
    connectionError: undefined,
    pairingPending: false,
    devices: [],
  } as any);
  seedBook();
});
afterEach(cleanup);

describe('ConnectionOverlay BLE pick token hygiene', () => {
  it('picking a KNOWN device overwrites the fields with that device book entry', async () => {
    await renderAndPick({ id: 'dev-b', name: 'BrambleNode' });
    await waitFor(() => expect(screen.getByLabelText('Auth Token')).toHaveValue('tokB'));
    expect(screen.getByLabelText(/name \(optional\)/i)).toHaveValue('Kitchen');
    expect(screen.getByLabelText(/remember this device/i)).toBeChecked();
  });

  it('picking an UNKNOWN device clears the token, unchecks remember, and takes its name', async () => {
    await renderAndPick({ id: 'dev-x', name: 'NewNode' });
    await waitFor(() => expect(screen.getByLabelText('Auth Token')).toHaveValue(''));
    expect(screen.getByLabelText(/remember this device/i)).not.toBeChecked();
    expect(screen.getByLabelText(/name \(optional\)/i)).toHaveValue('NewNode');
  });

  it('an unknown nameless device leaves the name empty rather than keeping the previous node name', async () => {
    await renderAndPick({ id: 'dev-y' });
    await waitFor(() => expect(screen.getByLabelText('Auth Token')).toHaveValue(''));
    expect(screen.getByLabelText(/name \(optional\)/i)).toHaveValue('');
  });

  it('the mount prefill does not promote a session-only token to Remember on', async () => {
    // The user saved this token with Remember OFF (sessionStorage). Pre-
    // checking the box from mere token presence would promote it to
    // localStorage on the next unnoticed Connect, defeating "leave off on
    // shared computers" on the one prefill path that skipped the entry's
    // remember flag.
    upsertDevice({ address: 'AAAA0001', name: 'Porch', transport: 'ble', remember: false, nowMs: 6 });
    setDeviceToken('AAAA0001', 'tokA', false);
    refreshDevices();
    render(<ConnectionOverlay />);
    fireEvent.click(screen.getByRole('button', { name: /^bluetooth$/i }));
    await waitFor(() => expect(screen.getByLabelText('Auth Token')).toHaveValue('tokA'));
    expect(screen.getByLabelText(/remember this device/i)).not.toBeChecked();
  });
});
