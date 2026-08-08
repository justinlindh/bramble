import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { act, cleanup, fireEvent, render, screen } from '@testing-library/react';
import { ConnectionOverlay } from '../../src/components/ConnectionOverlay';
import { useStore } from '../../src/store/index';
import { DEFAULT_CAPABILITIES } from '../../src/lib/connectionMode';
import type { ConnectionCapabilities } from '../../src/types/bramble';

vi.mock('../../src/store/actions', () => ({
  connect: vi.fn(),
  refreshDevices: vi.fn(),
  forgetSavedDevice: vi.fn(),
  renameSavedDevice: vi.fn(),
}));

const HOSTED: ConnectionCapabilities = { ...DEFAULT_CAPABILITIES };
const LOCAL: ConnectionCapabilities = { mode: 'local', localLanAllowed: true };

// The capabilities fetch lives in App's mount effect, so the overlay only ever
// sees its result arrive through the store. Driving that seam directly keeps
// the ordering between a user's click and the response explicit.
function capabilitiesArrive(c: ConnectionCapabilities): void {
  act(() => {
    useStore.getState().setConnectionCapabilities(c);
  });
}

function withWebSerial(): void {
  Object.defineProperty(navigator, 'serial', { value: {}, configurable: true });
}

function withWebBluetooth(): void {
  Object.defineProperty(navigator, 'bluetooth', { value: {}, configurable: true });
}

const button = (name: RegExp) => screen.getByRole('button', { name });

describe('ConnectionOverlay capability gating', () => {
  beforeEach(() => {
    vi.restoreAllMocks();
    localStorage.clear();
    useStore.setState({
      connectionState: 'disconnected',
      connectionError: undefined,
      connectionCapabilities: DEFAULT_CAPABILITIES,
      capabilitiesLoaded: false,
    });
  });

  afterEach(() => {
    vi.unstubAllGlobals();
    // jsdom ships neither API; the tests that add one must not leak it.
    Reflect.deleteProperty(navigator, 'serial');
    Reflect.deleteProperty(navigator, 'bluetooth');
    cleanup();
  });

  it('explains hosted-mode WiFi instead of dead-ending on a disabled button', () => {
    render(<ConnectionOverlay />);
    capabilitiesArrive(HOSTED);

    expect(screen.getByText('Hosted')).toBeInTheDocument();

    const wifi = button(/wifi/i);
    // Operable, and described by its caption rather than declared disabled:
    // pressing it is the only route to the explanation, so assistive tech must
    // be told it can be pressed.
    expect(wifi).not.toBeDisabled();
    expect(wifi).not.toHaveAttribute('aria-disabled');
    expect(wifi).toHaveAttribute('aria-describedby', 'transport-wifi-caption');
    expect(document.getElementById('transport-wifi-caption')).toHaveTextContent('Unavailable here');
    // aria-describedby supersedes a title, so shipping both would silently drop one.
    expect(wifi).not.toHaveAttribute('title');

    fireEvent.click(wifi);

    expect(wifi).toHaveAttribute('aria-pressed', 'true');
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
    capabilitiesArrive(HOSTED);

    // jsdom has no serial API by default.
    const usb = button(/usb/i);
    expect(usb).not.toBeDisabled();
    expect(usb).not.toHaveAttribute('aria-disabled');
    expect(usb).toHaveAttribute('aria-describedby', 'transport-serial-caption');
    fireEvent.click(usb);
    expect(screen.getByText('USB is not available in this browser')).toBeInTheDocument();

    cleanup();
    withWebSerial();
    useStore.setState({ connectionCapabilities: DEFAULT_CAPABILITIES, capabilitiesLoaded: false });

    render(<ConnectionOverlay />);
    capabilitiesArrive(HOSTED);
    expect(button(/usb/i)).not.toHaveAttribute('aria-describedby');
    expect(screen.queryByText('USB is not available in this browser')).toBeNull();
  });

  it('opens on the first transport that works', () => {
    withWebBluetooth();
    render(<ConnectionOverlay />);
    capabilitiesArrive(HOSTED);

    // USB comes first in the order but has no Web Serial here, so Bluetooth
    // wins and its connection form is what the user lands on.
    expect(button(/bluetooth/i)).toHaveAttribute('aria-pressed', 'true');
    expect(button(/usb/i)).toHaveAttribute('aria-pressed', 'false');
    expect(screen.getByRole('button', { name: 'Select device…' })).toBeInTheDocument();

    cleanup();
    withWebSerial();
    useStore.setState({ connectionCapabilities: DEFAULT_CAPABILITIES, capabilitiesLoaded: false });

    render(<ConnectionOverlay />);
    capabilitiesArrive(HOSTED);
    expect(button(/usb/i)).toHaveAttribute('aria-pressed', 'true');
    expect(button(/bluetooth/i)).toHaveAttribute('aria-pressed', 'false');
  });

  it('keeps a transport the user picked before capabilities resolved', () => {
    withWebSerial();
    render(<ConnectionOverlay />);

    // The pick happens while the capabilities request is still in flight.
    fireEvent.click(button(/wifi/i));
    expect(button(/wifi/i)).toHaveAttribute('aria-pressed', 'true');

    capabilitiesArrive(HOSTED);

    // USB is the transport the default would have chosen. The user's choice
    // outranks it, so the explanation they asked for stays on screen instead
    // of the selection being bounced away under them.
    expect(button(/wifi/i)).toHaveAttribute('aria-pressed', 'true');
    expect(button(/usb/i)).toHaveAttribute('aria-pressed', 'false');
    expect(screen.getByText('WiFi needs a direct connection to your node')).toBeInTheDocument();
  });

  it('falls back to the USB explainer, never the mock node, when nothing works', () => {
    // A saved IP would otherwise make WiFi the preferred opening transport.
    localStorage.setItem('bramble_wifi_ip', '192.0.2.10');
    render(<ConnectionOverlay />);
    capabilitiesArrive(HOSTED);

    // jsdom has neither Web Serial nor Web Bluetooth, and hosted mode rules
    // out WiFi, so no transport can reach a node.
    expect(button(/usb/i)).toHaveAttribute('aria-pressed', 'true');
    expect(screen.getByText('USB is not available in this browser')).toBeInTheDocument();
    expect(screen.getByText(/No transport in this browser can reach a node/)).toBeInTheDocument();
    expect(button(/mock node/i)).toHaveAttribute('aria-pressed', 'false');
  });

  it('states no restriction before the capabilities response arrives', () => {
    // A returning WiFi user: the hosted defaults in the store are a
    // placeholder, and asserting them would accuse this runtime of a
    // restriction it may not have.
    localStorage.setItem('bramble_wifi_ip', '192.0.2.10');
    render(<ConnectionOverlay />);

    expect(screen.queryByText('WiFi needs a direct connection to your node')).toBeNull();
    expect(button(/wifi/i)).not.toHaveAttribute('aria-describedby');
    expect(screen.getByLabelText('Node address')).toHaveValue('192.0.2.10');

    // Local mode confirms WiFi works, and the form the user was already
    // looking at stays put.
    capabilitiesArrive(LOCAL);
    expect(button(/wifi/i)).toHaveAttribute('aria-pressed', 'true');
    expect(screen.getByLabelText('Node address')).toHaveValue('192.0.2.10');
  });
});
