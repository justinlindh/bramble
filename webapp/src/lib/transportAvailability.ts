import type { ConnectionCapabilities } from '../types/bramble';
import type { Platform } from '../utils/platform';
import { LOCAL_LAN_UNAVAILABLE_REASON } from './connectionMode';

/** Desktop installers ride the webapp-vX.Y.Z release tags. */
export const DESKTOP_APP_URL = 'https://github.com/justinlindh/bramble/releases';

const DESKTOP_CTA = { label: 'Get the desktop app', href: DESKTOP_APP_URL } as const;

/**
 * The transports whose availability depends on the runtime or the browser.
 * The mock node is always available and deliberately has no entry here.
 */
export type GatedTransport = 'serial' | 'ble' | 'wifi';

export interface TransportUnavailable {
  available: false;
  /** Short label under the button. Kept uniform so the row stays aligned. */
  caption: string;
  heading: string;
  /** Why it cannot work here. */
  body: string;
  /** What does work here. Derived, never authored per case. */
  alternatives: string;
  cta?: { label: string; href: string };
}

export type TransportAvailability = { available: true } | TransportUnavailable;

export interface AvailabilityInput {
  capabilities: ConnectionCapabilities;
  hasSerial: boolean;
  hasBluetooth: boolean;
  platform: Platform;
}

const LABELS: Record<GatedTransport, string> = {
  serial: 'USB',
  ble: 'Bluetooth',
  wifi: 'WiFi',
};

const HOSTED_LAN_BODY =
  'Browsers block a public https page from opening an insecure connection to a device on your home network, so this hosted app cannot reach your node over WiFi.';

/**
 * The one sentence naming what still works. Generated from the availability
 * set rather than authored per case: authoring it per case is how copy ends
 * up telling an iOS user to try Bluetooth, which is the one thing iOS cannot
 * do.
 */
function alternativesSentence(available: GatedTransport[], platform: Platform): string {
  if (available.length === 0) {
    // With nothing available the body already carries the fix (a different
    // browser, or the desktop app, which the CTA repeats on desktop), so this
    // sentence only adds the mock node. Telling a desktop user to "connect
    // from a computer" would contradict the paragraph directly above it.
    return platform === 'desktop'
      ? 'No transport in this browser can reach a node. You can still explore the app with the mock node below.'
      : 'No transport in this browser can reach a node. Connect from a computer, or explore the app with the mock node below.';
  }
  const names = available.map(t => LABELS[t]);
  const list =
    names.length === 1 ? names[0] : `${names.slice(0, -1).join(', ')} and ${names[names.length - 1]}`;
  return `${list} ${names.length === 1 ? 'works' : 'work'} here.`;
}

function wifiBody(capabilities: ConnectionCapabilities, platform: Platform): string {
  // normalizeCapabilities() fills localLanReason with the sentinel default
  // when the server supplies none, so anything else is the server speaking
  // and wins over our copy.
  const supplied = capabilities.localLanReason;
  const reason = supplied && supplied !== LOCAL_LAN_UNAVAILABLE_REASON ? supplied : HOSTED_LAN_BODY;
  const fix =
    platform === 'desktop'
      ? 'The desktop app runs locally, where that restriction does not apply, and it finds nodes on your network automatically.'
      : 'Reaching a node over WiFi needs the desktop app on a computer.';
  return `${reason} ${fix}`;
}

function serialBody(platform: Platform): string {
  if (platform === 'desktop') {
    return 'Web Serial is how a browser talks to a USB device, and this browser does not implement it. Chrome or Edge works, as does the desktop app.';
  }
  return 'Browsers on phones and tablets cannot talk to USB devices. Connecting over USB needs a computer.';
}

function bleBody(platform: Platform): string {
  if (platform === 'desktop') {
    return 'This browser does not implement Web Bluetooth. Chrome or Edge works, as does the desktop app.';
  }
  if (platform === 'android') {
    return 'This browser does not implement Web Bluetooth. Chrome on Android works.';
  }
  return 'iOS browsers cannot use Bluetooth, and there is no iOS app.';
}

export function describeTransports(input: AvailabilityInput): Record<GatedTransport, TransportAvailability> {
  const { capabilities, hasSerial, hasBluetooth, platform } = input;

  const isAvailable: Record<GatedTransport, boolean> = {
    serial: hasSerial,
    ble: hasBluetooth,
    wifi: capabilities.localLanAllowed,
  };

  const order: GatedTransport[] = ['serial', 'ble', 'wifi'];
  const alternatives = alternativesSentence(order.filter(t => isAvailable[t]), platform);

  // The desktop app is only a usable answer on a desktop OS, so it is the
  // one thing gating the CTA.
  const withCta = platform === 'desktop';

  const unavailable = (caption: string, heading: string, body: string): TransportUnavailable => ({
    available: false,
    caption,
    heading,
    body,
    alternatives,
    ...(withCta ? { cta: { ...DESKTOP_CTA } } : {}),
  });

  return {
    serial: isAvailable.serial
      ? { available: true }
      : unavailable('Not in this browser', 'USB is not available in this browser', serialBody(platform)),
    ble: isAvailable.ble
      ? { available: true }
      : unavailable('Not in this browser', 'Bluetooth is not available in this browser', bleBody(platform)),
    wifi: isAvailable.wifi
      ? { available: true }
      : unavailable('Unavailable here', 'WiFi needs a direct connection to your node', wifiBody(capabilities, platform)),
  };
}
