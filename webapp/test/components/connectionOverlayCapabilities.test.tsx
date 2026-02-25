import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { cleanup, render, screen, waitFor } from '@testing-library/react';
import { ConnectionOverlay } from '../../src/components/ConnectionOverlay';
import { useStore } from '../../src/store/index';

vi.mock('../../src/store/actions', () => ({
  connect: vi.fn(),
}));

describe('ConnectionOverlay capability gating', () => {
  beforeEach(() => {
    vi.restoreAllMocks();
    useStore.setState({
      connectionState: 'disconnected',
      connectionError: undefined,
      manualDisconnect: false,
    });
  });

  afterEach(() => {
    vi.unstubAllGlobals();
    cleanup();
  });

  it('disables LAN direct connect in hosted mode with reason', async () => {
    vi.stubGlobal('fetch', vi.fn(async () => ({
      ok: true,
      json: async () => ({
        mode: 'hosted',
        localLanAllowed: false,
        proxyEnabled: false,
      }),
    })));

    render(<ConnectionOverlay />);

    expect(await screen.findByText('Hosted')).toBeInTheDocument();

    const wifiButton = screen.getByRole('button', { name: /wifi/i });
    await waitFor(() => expect(wifiButton).toBeDisabled());
    expect(wifiButton).toHaveAttribute('title', 'LAN direct connect is unavailable in hosted mode. Use USB or Bluetooth.');
  });

  it('enables USB only when navigator.serial exists', () => {
    // jsdom has no serial API by default
    render(<ConnectionOverlay />);
    expect(screen.getByRole('button', { name: /usb/i })).toBeDisabled();

    cleanup();
    Object.defineProperty(navigator, 'serial', {
      value: {},
      configurable: true,
    });

    render(<ConnectionOverlay />);
    expect(screen.getByRole('button', { name: /usb/i })).not.toBeDisabled();
  });
});
