import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import { cleanup, fireEvent, render, screen } from '@testing-library/react';
import { ConnectionOverlay } from '../ConnectionOverlay';
import { useStore } from '../../store/index';

// WiFi only renders when local LAN is allowed; set the full capabilities shape.
function allowWifi() {
  useStore.getState().setConnectionCapabilities({
    mode: 'local',
    localLanAllowed: true,
    localLanReason: '',
  });
}

// Select the WiFi transport (the form is gated behind the picker).
function selectWifi() {
  fireEvent.click(screen.getByRole('button', { name: /wifi/i }));
}

beforeEach(() => {
  localStorage.clear();
  sessionStorage.clear();
  allowWifi();
});
afterEach(cleanup);

describe('ConnectionOverlay new-device form', () => {
  it('shows the Auth Token field by default when WiFi is selected (no collapse)', () => {
    render(<ConnectionOverlay />);
    selectWifi();
    // No "Authentication" toggle to expand: the token field is present inline.
    expect(screen.queryByRole('button', { name: /^authentication$/i })).toBeNull();
    expect(screen.getByLabelText(/auth token/i)).toBeInTheDocument();
  });

  it('renders a Remember this device checkbox', () => {
    render(<ConnectionOverlay />);
    selectWifi();
    expect(screen.getByLabelText(/remember this device/i)).toBeInTheDocument();
  });

  it('renders an optional Name field', () => {
    render(<ConnectionOverlay />);
    selectWifi();
    expect(screen.getByLabelText(/name/i)).toBeInTheDocument();
  });
});

// Web Bluetooth is not present in jsdom by default; add it directly on the
// real navigator (rather than replacing the whole object) so the Bluetooth
// picker button is enabled without disturbing other navigator-dependent code.
function selectBluetooth() {
  fireEvent.click(screen.getByRole('button', { name: /bluetooth/i }));
}

describe('ConnectionOverlay Bluetooth auth token field', () => {
  beforeEach(() => {
    (navigator as unknown as { bluetooth: unknown }).bluetooth = {};
  });

  afterEach(() => {
    delete (navigator as unknown as { bluetooth?: unknown }).bluetooth;
  });

  it('shows the Auth Token field when Bluetooth is selected', () => {
    render(<ConnectionOverlay />);
    selectBluetooth();
    expect(screen.getByLabelText(/auth token/i)).toBeInTheDocument();
  });

  it('does not show the WiFi-only node address field for Bluetooth', () => {
    render(<ConnectionOverlay />);
    selectBluetooth();
    expect(screen.queryByLabelText(/node address/i)).toBeNull();
  });

  it('shows a Remember this device checkbox for Bluetooth so the token can be saved', () => {
    render(<ConnectionOverlay />);
    selectBluetooth();
    expect(screen.getByLabelText(/remember this device/i)).toBeInTheDocument();
  });
});
