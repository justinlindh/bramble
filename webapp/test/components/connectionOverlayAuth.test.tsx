import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { cleanup, fireEvent, render, screen } from '@testing-library/react';
import { ConnectionOverlay } from '../../src/components/ConnectionOverlay';
import { useStore } from '../../src/store/index';

const connectMock = vi.fn();

vi.mock('../../src/store/actions', () => ({
  connect: (...args: unknown[]) => connectMock(...args),
}));

describe('ConnectionOverlay auth token flow', () => {
  beforeEach(() => {
    localStorage.clear();
    sessionStorage.clear();
    connectMock.mockReset();
    useStore.setState({
      connectionState: 'disconnected',
      connectionError: undefined,
      manualDisconnect: false,
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

  it('shows token controls only for WiFi transport', () => {
    render(<ConnectionOverlay />);
    expect(screen.queryByText('Authentication')).not.toBeInTheDocument();

    fireEvent.click(screen.getByRole('button', { name: /wifi/i }));
    expect(screen.getByRole('button', { name: 'Authentication' })).toBeInTheDocument();

    fireEvent.click(screen.getByRole('button', { name: 'Authentication' }));
    expect(screen.getByLabelText('Auth Token (optional)')).toBeInTheDocument();
  });

  it('passes token in connect flow and persists it', () => {
    render(<ConnectionOverlay />);
    fireEvent.click(screen.getByRole('button', { name: /wifi/i }));

    fireEvent.change(screen.getByLabelText(/node address/i), { target: { value: '192.168.4.1' } });
    fireEvent.click(screen.getByRole('button', { name: 'Authentication' }));
    fireEvent.change(screen.getByLabelText('Auth Token (optional)'), { target: { value: 'secret-token' } });

    fireEvent.click(screen.getByRole('button', { name: 'Connect' }));

    expect(connectMock).toHaveBeenCalledWith('wifi', expect.objectContaining({ token: 'secret-token' }));
    expect(localStorage.getItem('bramble_wifi_ip')).toBe('192.168.4.1');
    // S19: token now stored in sessionStorage, not localStorage
    expect(sessionStorage.getItem('bramble_wifi_token')).toBe('secret-token');
    expect(localStorage.getItem('bramble_wifi_token')).toBeNull();
  });

  it('highlights token field on auth errors', () => {
    useStore.setState({ connectionError: 'Authentication required. Enter your device token.' } as any);
    render(<ConnectionOverlay />);

    fireEvent.click(screen.getByRole('button', { name: /wifi/i }));
    fireEvent.click(screen.getByRole('button', { name: 'Authentication' }));

    const tokenInput = screen.getByLabelText('Auth Token (optional)');
    expect(tokenInput.className).toMatch(/authErrorField/);
  });
});
