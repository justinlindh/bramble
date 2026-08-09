import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { cleanup, fireEvent, render, screen, waitFor } from '@testing-library/react';
import { ConnectionOverlay } from '../ConnectionOverlay';
import { useStore } from '../../store/index';
import { upsertDevice, setDeviceToken } from '../../lib/deviceBook';
import * as actions from '../../store/actions';

// The saved-device rows drive the form: a row click selects the transport,
// prefills the form from the book (so a failure leaves the right values on
// screen), and only then connects. Dead clicks (wifi with no lastIp, serial)
// and off-screen errors were the old behavior these tests pin against.

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

describe('ConnectionOverlay favorites', () => {
  it('a BLE favorite prefills the Bluetooth form and connects with the stored token', async () => {
    upsertDevice({ address: 'BBBB0002', name: 'Kitchen', transport: 'ble', remember: true, nowMs: 2 });
    setDeviceToken('BBBB0002', 'tokB', true);
    const spy = vi.spyOn(actions, 'connect').mockResolvedValue();
    render(<ConnectionOverlay />);
    fireEvent.click(screen.getByRole('button', { name: /connect to Kitchen/i }));
    await waitFor(() => expect(spy).toHaveBeenCalledWith('ble', expect.objectContaining({
      token: 'tokB', remember: true, name: 'Kitchen', expectAddressHex: 'BBBB0002',
    })));
    // The form followed the click: BLE tab active, token and name prefilled.
    expect(screen.getByLabelText('Auth Token')).toHaveValue('tokB');
    expect(screen.getByLabelText(/name \(optional\)/i)).toHaveValue('Kitchen');
    expect(screen.getByLabelText(/remember this device/i)).toBeChecked();
  });

  it('a wifi favorite with a lastIp prefills the WiFi form and connects', async () => {
    upsertDevice({ address: 'DEADBEEF', name: 'V4', lastIp: '198.51.100.146', transport: 'wifi', remember: true, nowMs: 1 });
    setDeviceToken('DEADBEEF', 'tokW', true);
    const spy = vi.spyOn(actions, 'connect').mockResolvedValue();
    render(<ConnectionOverlay />);
    fireEvent.click(screen.getByRole('button', { name: /connect to V4/i }));
    await waitFor(() => expect(spy).toHaveBeenCalledWith('wifi', expect.objectContaining({
      ip: '198.51.100.146', token: 'tokW', expectAddressHex: 'DEADBEEF', remember: true,
    })));
    expect(screen.getByLabelText(/node address/i)).toHaveValue('198.51.100.146');
    expect(screen.getByLabelText('Auth Token')).toHaveValue('tokW');
  });

  it('a wifi favorite with NO lastIp prefills, switches the tab, and focuses the address field', async () => {
    upsertDevice({ address: 'CAFED00D', name: 'Relay', transport: 'wifi', remember: true, nowMs: 1 });
    setDeviceToken('CAFED00D', 'tokA', true);
    const spy = vi.spyOn(actions, 'connect').mockResolvedValue();
    render(<ConnectionOverlay />);
    fireEvent.click(screen.getByRole('button', { name: /connect to Relay/i }));
    // No address to dial: the user completes it. Connecting anyway was the
    // old silent dead click; focusing the empty field is the visible cue for
    // what happened and what to do next.
    expect(spy).not.toHaveBeenCalled();
    expect(screen.getByLabelText(/node address/i)).toHaveValue('');
    expect(screen.getByLabelText('Auth Token')).toHaveValue('tokA');
    expect(screen.getByLabelText(/name \(optional\)/i)).toHaveValue('Relay');
    await waitFor(() => expect(screen.getByLabelText(/node address/i)).toHaveFocus());
  });

  it('an ip-less row click does not steal an earlier form error into the row slot', async () => {
    // The row click above never attempts a connection, so a leftover error
    // from a previous FORM attempt must stay attributed to the form: moving
    // it up would pin someone else's failure on a row that did nothing.
    upsertDevice({ address: 'CAFED00D', name: 'Relay', transport: 'wifi', remember: true, nowMs: 1 });
    vi.spyOn(actions, 'connect').mockResolvedValue();
    render(<ConnectionOverlay />);
    useStore.getState().setConnectionState('disconnected', 'earlier form boom');
    fireEvent.click(screen.getByRole('button', { name: /connect to Relay/i }));
    const err = await screen.findByText(/earlier form boom/);
    const heading = screen.getByText(/add a device/i);
    expect(err.compareDocumentPosition(heading) & Node.DOCUMENT_POSITION_PRECEDING).toBeTruthy();
  });

  it('a serial favorite connects over serial with the identity guard', async () => {
    upsertDevice({ address: 'FEEDF00D', name: 'Bench node', transport: 'serial', remember: false, nowMs: 1 });
    const spy = vi.spyOn(actions, 'connect').mockResolvedValue();
    render(<ConnectionOverlay />);
    fireEvent.click(screen.getByRole('button', { name: /connect to Bench node/i }));
    // expectAddressHex: serial ports are indistinguishable in the browser
    // picker, so without the guard a wrong pick would create a book entry
    // for another node carrying this row's name.
    await waitFor(() => expect(spy).toHaveBeenCalledWith('serial', expect.objectContaining({
      name: 'Bench node', expectAddressHex: 'FEEDF00D',
    })));
  });

  it('shows the pairing banner next to the list when the attempt came from a row', async () => {
    // A saved-row BLE reconnect can still need re-pairing (OS bond removed,
    // new browser profile). The spinner is at the row, so the type-the-code
    // instruction must be too: the bottom slot is a screenful below.
    upsertDevice({ address: 'BBBB0002', name: 'Kitchen', transport: 'ble', remember: true, nowMs: 2 });
    vi.spyOn(actions, 'connect').mockImplementation(() => {
      useStore.setState({ connectionState: 'connecting', pairingPending: true } as any);
      return new Promise(() => {});
    });
    render(<ConnectionOverlay />);
    fireEvent.click(screen.getByRole('button', { name: /connect to Kitchen/i }));
    const banner = await screen.findByRole('alert');
    expect(banner.textContent).toMatch(/pairing code/i);
    const heading = screen.getByText(/add a device/i);
    expect(banner.compareDocumentPosition(heading) & Node.DOCUMENT_POSITION_FOLLOWING).toBeTruthy();
  });

  it('renders a row-initiated error directly under the device list, not in the bottom slot', async () => {
    upsertDevice({ address: 'DEADBEEF', name: 'V4', lastIp: '198.51.100.146', transport: 'wifi', remember: true, nowMs: 1 });
    vi.spyOn(actions, 'connect').mockImplementation(async () => {
      useStore.getState().setConnectionState('disconnected', 'row boom');
    });
    render(<ConnectionOverlay />);
    fireEvent.click(screen.getByRole('button', { name: /connect to V4/i }));
    const err = await screen.findByText(/row boom/);
    const heading = screen.getByText(/add a device/i);
    // The error sits above the "Add a device" heading, next to the row that
    // caused it; the old bottom slot put it a screenful below the row.
    expect(err.compareDocumentPosition(heading) & Node.DOCUMENT_POSITION_FOLLOWING).toBeTruthy();
  });

  it('renders a form-initiated error in the bottom slot under the form', async () => {
    upsertDevice({ address: 'DEADBEEF', name: 'V4', lastIp: '198.51.100.146', transport: 'wifi', remember: true, nowMs: 1 });
    vi.spyOn(actions, 'connect').mockImplementation(async () => {
      useStore.getState().setConnectionState('disconnected', 'form boom');
    });
    render(<ConnectionOverlay />);
    fireEvent.click(screen.getByRole('button', { name: /^connect$/i }));
    const err = await screen.findByText(/form boom/);
    const heading = screen.getByText(/add a device/i);
    expect(err.compareDocumentPosition(heading) & Node.DOCUMENT_POSITION_PRECEDING).toBeTruthy();
  });

  it('locks the device rows while a connect is in flight', () => {
    upsertDevice({ address: 'DEADBEEF', name: 'V4', lastIp: '198.51.100.146', transport: 'wifi', remember: true, nowMs: 1 });
    useStore.setState({ connectionState: 'connecting' } as any);
    render(<ConnectionOverlay />);
    expect(screen.getByRole('button', { name: /connect to V4/i })).toBeDisabled();
    expect(screen.getByRole('button', { name: /rename V4/i })).toBeDisabled();
    expect(screen.getByRole('button', { name: /forget V4/i })).toBeDisabled();
  });
});
