import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { cleanup, fireEvent, render, screen, waitFor } from '@testing-library/react';
import { ConnectionOverlay } from '../../src/components/ConnectionOverlay';
import { useStore } from '../../src/store/index';

vi.mock('../../src/store/actions', () => ({
  connect: vi.fn(),
  refreshDevices: vi.fn(),
  forgetSavedDevice: vi.fn(),
  renameSavedDevice: vi.fn(),
}));

describe('ConnectionOverlay capability gating', () => {
  beforeEach(() => {
    vi.restoreAllMocks();
    useStore.setState({
      connectionState: 'disconnected',
      connectionError: undefined,
    });
  });

  afterEach(() => {
    vi.unstubAllGlobals();
    cleanup();
  });

  it('explains hosted-mode WiFi instead of dead-ending on a disabled button', async () => {
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
    await waitFor(() => expect(wifiButton).toHaveAttribute('aria-disabled', 'true'));
    // Selectable, unlike a disabled button, because selecting it is what
    // shows the explanation.
    expect(wifiButton).not.toBeDisabled();

    fireEvent.click(wifiButton);

    expect(screen.getByText('WiFi needs a direct connection to your node')).toBeInTheDocument();
    expect(screen.getByText(/cannot reach your node over WiFi/)).toBeInTheDocument();
    // jsdom reports a desktop user agent, so the desktop app is a usable
    // answer here and the CTA is offered.
    expect(screen.getByRole('link', { name: 'Get the desktop app' })).toHaveAttribute(
      'href',
      'https://github.com/justinlindh/bramble/releases',
    );
    // Nothing offers an action that cannot succeed.
    expect(screen.queryByRole('button', { name: 'Connect' })).toBeNull();
  });

  it('marks USB unavailable only when navigator.serial is missing', () => {
    render(<ConnectionOverlay />);
    // jsdom has no serial API by default
    expect(screen.getByRole('button', { name: /usb/i })).toHaveAttribute('aria-disabled', 'true');

    cleanup();
    Object.defineProperty(navigator, 'serial', {
      value: {},
      configurable: true,
    });

    render(<ConnectionOverlay />);
    expect(screen.getByRole('button', { name: /usb/i })).not.toHaveAttribute('aria-disabled', 'true');
  });
});
