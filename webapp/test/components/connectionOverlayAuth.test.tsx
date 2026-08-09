import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { cleanup, fireEvent, render, screen } from '@testing-library/react';
import { ConnectionOverlay } from '../../src/components/ConnectionOverlay';
import { useStore } from '../../src/store/index';

const connectMock = vi.fn();

vi.mock('../../src/store/actions', () => ({
  connect: (...args: unknown[]) => connectMock(...args),
  refreshDevices: vi.fn(),
  forgetSavedDevice: vi.fn(),
  renameSavedDevice: vi.fn(),
}));

describe('ConnectionOverlay auth token flow', () => {
  beforeEach(() => {
    localStorage.clear();
    sessionStorage.clear();
    connectMock.mockReset();
    useStore.setState({
      connectionState: 'disconnected',
      connectionError: undefined,
      connectionCapabilities: {
        mode: 'local',
        localLanAllowed: true,
        localLanReason: '',
        proxyEnabled: false,
      },
    } as any);
  });

  afterEach(() => {
    cleanup();
    vi.unstubAllGlobals();
  });

  it('shows the auth token field inline for WiFi (no collapse toggle)', () => {
    render(<ConnectionOverlay />);
    // The old collapsed "Authentication" toggle is gone; selecting WiFi shows the field directly.
    fireEvent.click(screen.getByRole('button', { name: /wifi/i }));
    expect(screen.queryByRole('button', { name: 'Authentication' })).not.toBeInTheDocument();
    expect(screen.getByLabelText('Auth Token')).toBeInTheDocument();
  });

  it('passes the token to the connect flow', () => {
    render(<ConnectionOverlay />);
    fireEvent.click(screen.getByRole('button', { name: /wifi/i }));

    fireEvent.change(screen.getByLabelText(/node address/i), { target: { value: '192.168.4.1' } });
    fireEvent.change(screen.getByLabelText('Auth Token'), { target: { value: 'secret-token' } });

    fireEvent.click(screen.getByRole('button', { name: 'Connect' }));

    expect(connectMock).toHaveBeenCalledWith('wifi', expect.objectContaining({ token: 'secret-token', ip: '192.168.4.1' }));
    expect(localStorage.getItem('bramble_wifi_ip')).toBe('192.168.4.1');
    // The legacy single-token key is no longer written; the device book persists a
    // token per-address post-connect (opt-in), not from the form submit.
    expect(sessionStorage.getItem('bramble_wifi_token')).toBeNull();
    expect(localStorage.getItem('bramble_wifi_token')).toBeNull();
  });

  it('highlights token field on auth errors', () => {
    useStore.setState({ connectionError: 'Authentication required. Enter your device token.' } as any);
    render(<ConnectionOverlay />);

    fireEvent.click(screen.getByRole('button', { name: /wifi/i }));

    const tokenInput = screen.getByLabelText('Auth Token');
    expect(tokenInput.className).toMatch(/authErrorField/);
  });

  it('shows a visual spinner while BLE scanning is in progress', () => {
    useStore.setState({ connectionState: 'connecting' } as any);
    vi.stubGlobal('navigator', { ...navigator, bluetooth: {} });
    render(<ConnectionOverlay />);

    fireEvent.click(screen.getByRole('button', { name: /bluetooth/i }));
    // The spinner icon is decoration (aria-hidden): a fixed 'Scanning' label
    // on it contradicted the visible text the moment that flipped to
    // Pairing…. The visible connecting label carries the state instead.
    const connectBtn = screen.getByRole('button', { name: /scanning/i });
    const icon = connectBtn.querySelector('[class*="spinnerIcon"]');
    expect(icon).not.toBeNull();
    expect(icon).toHaveAttribute('aria-hidden', 'true');
  });
});
