import { useEffect, useMemo, useState } from 'react';
import { connect, refreshDevices } from '../store/actions';
import { BLETransport } from '../transport/BLETransport';
import { useStore } from '../store/index';
import { getDeviceToken, type SavedDevice } from '../lib/deviceBook';
import { isAuthError } from '../lib/errors';
import { isEmbeddedShell, describePlatform } from '../utils/platform';
import {
  describeTransports,
  type GatedTransport,
  type TransportAvailability,
  type TransportUnavailable,
} from '../lib/transportAvailability';
import type { TransportType } from '../types/bramble';
import { IconUsb, IconBluetooth, IconMonitor, IconWifi, IconWarning } from './Icons';
import { DeviceList } from './DeviceList';
import { NearbyNodes } from './NearbyNodes';
import type { NearbyNode } from '../lib/nearbyNodes';
import { TransportUnavailableNotice } from './TransportUnavailableNotice';
import styles from './ConnectionOverlay.module.css';

const WIFI_IP_KEY = 'bramble_wifi_ip';

export function buildWifiUrl(ip: string, protocol: string, host: string): string {
  let url: string;
  if (ip.includes('://')) url = ip;
  // Embedded shells talk to the node directly regardless of page origin.
  // The Android shell serves from https://appassets.androidplatform.net,
  // which would otherwise take the hosted wss-proxy branch below.
  else if (isEmbeddedShell()) url = `ws://${ip}/ws`;
  else if (protocol === 'https:') url = `wss://${host}/proxy/${ip}`;
  else url = `ws://${ip}/ws`;

  // Token is NOT embedded in the URL (NEW-SEC-6): it rides the WS subprotocol.
  return url;
}

// One-click connect to a device-book entry. The IP is a parameter because the
// caller may know a fresher one than the book (mDNS discovery vs saved lastIp);
// expectAddressHex keeps the DHCP guard: connect() drops the session if that
// IP now answers as a different node.
export function connectToSavedDevice(d: SavedDevice, ip: string): Promise<void> {
  const tok = getDeviceToken(d.address);
  const url = buildWifiUrl(ip, location.protocol, location.host);
  return connect('wifi', {
    url,
    token: tok || undefined,
    ip,
    remember: d.remember,
    name: d.name,
    expectAddressHex: d.address,
  });
}

// Reconnect to a saved Bluetooth device, reusing the stored token and the
// stored BLE identity. How silent this is depends on the shell:
// pickDevice(expected) resolves without a chooser only where the platform
// provides a hook (Electron: main auto-selects the matching candidate;
// Android polyfill: direct connect to the stored device id). A plain browser
// has no such hook and still shows the system chooser. pickDevice also falls
// back to the chooser when the identity is missing or stale, and
// expectAddressHex still verifies the node is the one the user chose,
// dropping the session on a mismatch.
export async function connectToSavedBleDevice(d: SavedDevice): Promise<void> {
  const tok = getDeviceToken(d.address);
  let device: BluetoothDevice | undefined;
  if (d.bleDeviceId || d.bleDeviceName) {
    try {
      device = await BLETransport.pickDevice({ id: d.bleDeviceId, name: d.bleDeviceName });
    } catch { /* fall through to the chooser inside connect() */ }
  }
  await connect('ble', {
    token: tok || undefined,
    remember: d.remember,
    name: d.name,
    expectAddressHex: d.address,
    bleDevice: device,
  });
}

export function connectingLabelFor(transportType: TransportType, pairingPending = false): string {
  // While the OS passkey prompt is up, pairing is the gating step whatever
  // the transport label would otherwise say: 'Scanning…' reads as a stall.
  if (pairingPending) return 'Pairing…';
  if (transportType === 'ble') return 'Scanning…';
  if (transportType === 'serial') return 'Opening serial…';
  if (transportType === 'wifi') return 'Handshaking…';
  return 'Connecting…';
}

// Connection policy: never auto-connect. The user presses Connect or picks a
// saved device. The "last IP" below is only a convenience prefill for the form
// (a single global value, distinct from the per-address device book).
function loadSavedIp(): string {
  try { return localStorage.getItem(WIFI_IP_KEY) || ''; } catch { return ''; }
}

function saveLastIp(ip: string): void {
  try { localStorage.setItem(WIFI_IP_KEY, ip); } catch { /* noop */ }
}

const TRANSPORT_CHOICES: Array<{
  id: GatedTransport;
  label: string;
  Icon: typeof IconUsb;
  title: string;
}> = [
  { id: 'serial', label: 'USB', Icon: IconUsb, title: 'Connect via USB cable' },
  { id: 'ble', label: 'Bluetooth', Icon: IconBluetooth, title: 'Connect via Bluetooth Low Energy' },
  { id: 'wifi', label: 'WiFi', Icon: IconWifi, title: 'Connect directly to your Bramble node over LAN WiFi' },
];

// Nothing is declared unavailable before the capabilities fetch resolves.
// WiFi's verdict comes straight from that response, and every explainer's
// "what does work here" sentence is derived from the whole set, so an earlier
// paint would state a restriction the app has not determined yet.
const UNDETERMINED: Record<GatedTransport, TransportAvailability> = {
  serial: { available: true },
  ble: { available: true },
  wifi: { available: true },
};

/** Ties an unavailable transport's button to the caption naming the reason. */
function captionId(id: GatedTransport): string {
  return `transport-${id}-caption`;
}

export function ConnectionOverlay() {
  const savedIp = loadSavedIp();
  const [transportType, setTransportType] = useState<TransportType>(savedIp ? 'wifi' : 'serial');
  const [wifiIp, setWifiIp] = useState(savedIp);
  const [wifiToken, setWifiToken] = useState('');
  const [wifiRemember, setWifiRemember] = useState(false);
  const [wifiName, setWifiName] = useState('');
  const [showToken, setShowToken] = useState(false);
  const [bleToken, setBleToken] = useState('');
  const [showBleToken, setShowBleToken] = useState(false);
  const [bleRemember, setBleRemember] = useState(false);
  const [bleName, setBleName] = useState('');
  const devices = useStore(s => s.devices);
  const connectionState = useStore(s => s.connectionState);
  const connectionError = useStore(s => s.connectionError);
  const connectionCapabilities = useStore(s => s.connectionCapabilities);
  const capabilitiesLoaded = useStore(s => s.capabilitiesLoaded);
  const pairingPending = useStore(s => s.pairingPending);

  const isConnecting = connectionState === 'connecting';
  const authError = isAuthError(connectionError);

  // Which surface started the attempt decides where its error renders: a
  // row-initiated error belongs next to the row (the bottom slot put it a
  // screenful below, off-screen on phones), a form-initiated error stays in
  // the bottom slot under the form.
  const [errorFrom, setErrorFrom] = useState<'row' | 'form'>('form');
  // Address of the row whose connect is in flight, for its busy spinner.
  const [rowBusyAddress, setRowBusyAddress] = useState<string | null>(null);

  useEffect(() => {
    refreshDevices();
  }, []);

  // Prefill the BLE token and name from the most recently used Bluetooth
  // device so a returning user does not retype the token. BLE cannot key by
  // address before connecting (the node is chosen in the system picker), so
  // the most-recent saved BLE device is the best available default.
  useEffect(() => {
    const lastBle = devices.find(d => d.transport === 'ble');
    if (!lastBle) return;
    const saved = getDeviceToken(lastBle.address);
    setBleToken(prev => (prev ? prev : saved));
    setBleName(prev => (prev ? prev : lastBle.name));
    // Reflect the entry's remember flag, not mere token presence: a token
    // saved with Remember off lives in sessionStorage, and pre-checking the
    // box for it would promote it to localStorage on the next unnoticed
    // Connect (the pick and row paths already honor the flag).
    if (saved) setBleRemember(lastBle.remember);
  }, [devices]);

  const [bleDevice, setBleDevice] = useState<BluetoothDevice | null>(null);
  const [blePickError, setBlePickError] = useState<string | null>(null);

  const handlePickBleDevice = async () => {
    setBlePickError(null);
    try {
      const device = await BLETransport.pickDevice();
      setBleDevice(device);
      // The picked device OWNS the fields from here: a known identity loads
      // its book state over whatever was typed, and an unknown one clears
      // them. The old sticky prefill silently kept the previous node's token
      // (which then auth-failed looking like a pairing problem) and could
      // label node B with node A's name.
      const known = devices.find(d => d.bleDeviceId === device.id || (device.name && d.bleDeviceName === device.name));
      if (known) {
        const saved = getDeviceToken(known.address);
        setBleToken(saved);
        setBleRemember(!!saved && known.remember);
        setBleName(known.name);
      } else {
        setBleToken('');
        setBleRemember(false);
        setBleName(device.name ?? '');
      }
    } catch (e) {
      const msg = (e as Error)?.message ?? '';
      // Cancelling the chooser is not an error state.
      if (!/cancel/i.test(msg)) setBlePickError(msg);
    }
  };

  const handleConnect = () => {
    setErrorFrom('form');
    if (transportType === 'wifi') {
      const ip = wifiIp.trim();
      const token = wifiToken.trim();
      if (!ip) return;
      // The device book owns per-device tokens (saved by address post-connect);
      // here we only persist the IP so the empty form prefills it next time.
      saveLastIp(ip);
      const url = buildWifiUrl(ip, location.protocol, location.host);
      connect(transportType, { url, token: token || undefined, ip, remember: wifiRemember, name: wifiName.trim() || undefined });
    } else if (transportType === 'ble') {
      const token = bleToken.trim();
      connect(transportType, {
        token: token || undefined,
        remember: bleRemember,
        name: bleName.trim() || undefined,
        bleDevice: bleDevice ?? undefined,
      });
    } else {
      connect(transportType);
    }
  };

  const availability = useMemo(
    () =>
      capabilitiesLoaded
        ? describeTransports({
            capabilities: connectionCapabilities,
            hasSerial: 'serial' in navigator,
            hasBluetooth: 'bluetooth' in navigator,
            platform: describePlatform(),
          })
        : UNDETERMINED,
    [connectionCapabilities, capabilitiesLoaded],
  );

  const runtimeBadge = connectionCapabilities.mode === 'local' ? 'Local LAN' : 'Hosted';

  const selectedEntry = transportType === 'websocket' ? { available: true as const } : availability[transportType];
  const selectedUnavailable: TransportUnavailable | null = selectedEntry.available ? null : selectedEntry;

  // The default lands on something that works. A saved IP means the user has
  // connected over WiFi before, so prefer it when WiFi is allowed. When
  // nothing works, which is the iOS hosted case, fall back to USB so its
  // explainer is on screen immediately: the mock node is never auto-selected.
  // Until capabilities resolve there is nothing to pick against, and moving the
  // selection off the user's saved preference on a guess is what made the card
  // flip mid-paint.
  const [userPicked, setUserPicked] = useState(false);
  useEffect(() => {
    if (userPicked || !capabilitiesLoaded) return;
    const order: GatedTransport[] = savedIp ? ['wifi', 'serial', 'ble'] : ['serial', 'ble', 'wifi'];
    setTransportType(order.find(t => availability[t].available) ?? 'serial');
  }, [availability, capabilitiesLoaded, userPicked, savedIp]);

  const pickTransport = (t: TransportType) => {
    setUserPicked(true);
    setTransportType(t);
  };

  // A saved-device row drives the form: select the transport tab, prefill
  // the fields from the book (so a failure leaves the RIGHT device's values
  // on screen for editing), then connect where an address is known. The
  // wifi-without-lastIp and serial rows used to be silent dead clicks.
  const handleRowConnect = async (d: SavedDevice) => {
    setUserPicked(true);
    const tok = getDeviceToken(d.address);
    if (d.transport === 'ble') {
      setErrorFrom('row');
      setTransportType('ble');
      setBleToken(tok);
      setBleRemember(!!tok && d.remember);
      setBleName(d.name);
      setRowBusyAddress(d.address);
      try { await connectToSavedBleDevice(d); } finally { setRowBusyAddress(null); }
    } else if (d.transport === 'wifi') {
      setTransportType('wifi');
      setWifiIp(d.lastIp);
      setWifiToken(tok);
      setWifiRemember(!!tok && d.remember);
      setWifiName(d.name);
      // No saved address means nothing to dial: leave the prefilled form for
      // the user to complete, and focus the empty field as the visible cue
      // (the form sits below the list, so a silent return reads as a dead
      // click). errorFrom stays untouched here: no attempt ran, and claiming
      // 'row' would pin a leftover form error on a row that did nothing.
      if (!d.lastIp) {
        setTimeout(() => document.getElementById('wifi-ip')?.focus(), 0);
        return;
      }
      setErrorFrom('row');
      setRowBusyAddress(d.address);
      try { await connectToSavedDevice(d, d.lastIp); } finally { setRowBusyAddress(null); }
    } else {
      // Serial: the browser's port picker appearing next is correct behavior.
      // expectAddressHex guards identity: ports are indistinguishable in the
      // picker, and without it a wrong pick would create a book entry for
      // another node carrying this row's name.
      setErrorFrom('row');
      setTransportType('serial');
      setRowBusyAddress(d.address);
      try {
        await connect('serial', { name: d.name, expectAddressHex: d.address });
      } finally { setRowBusyAddress(null); }
    }
  };

  // The BLE hint carries the pairing-code warning STATICALLY on purpose:
  // Chrome blocks the connect inside its own pairing dialog with no signal
  // the app can detect, so for Chrome users this copy is the only warning
  // that a code may appear on the node. The dynamic banner below only covers
  // stacks that reject writes while the prompt is up.
  // Fail-fast stacks (BlueZ) reject writes while the OS passkey prompt is
  // up, which the transport reports as pairingPending: point the user at the
  // prompt instead of looking hung. Rendered next to whichever surface
  // started the attempt (row slot or bottom slot), same as errors.
  const pairingBanner = isConnecting && pairingPending ? (
    <div className={styles.pairingBanner} role="alert">
      <strong>Your node is showing a pairing code.</strong>{' '}
      Enter that code in the browser prompt to finish connecting.
    </div>
  ) : null;

  const hints: Record<TransportType, string> = {
    serial: 'Connect your Bramble node via USB cable, then click Connect.',
    ble: 'Enable Bluetooth on your device, then click Connect to scan. First-time connections may show a pairing code on the node: type it into the browser prompt to finish.',
    websocket: 'Connects to the local mock node for development and demos.',
    wifi: 'The node must be on the same network (Station mode), or connect to its hotspot first (AP mode).',
  };

  return (
    <div className={styles.overlay}>
      <div className={styles.card}>
        {/* Relative path: embedded shells serve the app from a subpath
            (appassets .../assets/webapp/, electron file://), where an
            absolute /bramble-logo.png resolves outside the bundle. */}
        <img src="./bramble-logo.png" alt="Bramble" className={styles.logoImg} />
        <h1 className={styles.title}>Bramble</h1>
        <p className={styles.subtitle}>LoRa mesh companion</p>
        <div className={styles.metaRow}>
          <span className={styles.version}>{__APP_VERSION__}</span>
          <span className={styles.runtimeBadge} title={`Runtime context: ${runtimeBadge}`}>{runtimeBadge}</span>
        </div>

        <DeviceList
          onConnect={handleRowConnect}
          busyAddress={rowBusyAddress}
          disabled={isConnecting}
        />

        {/* Row-initiated feedback renders here, next to the row that caused
            it: the bottom slot is a screenful below the list, off-screen on
            phones. That applies to the pairing banner too, since a saved-row
            BLE reconnect can still need re-pairing (OS bond removed, new
            browser profile) and its spinner is up here. */}
        {errorFrom === 'row' && pairingBanner}
        {connectionError && errorFrom === 'row' && (
          <div className={styles.error}>
            <span><IconWarning size={14} /> {connectionError}</span>
          </div>
        )}

        {devices.length > 0 && (
          <h3 className={styles.sectionHeading}>Add a device</h3>
        )}

        <div className={styles.transportSelect}>
          {TRANSPORT_CHOICES.map(({ id, label, Icon, title }) => {
            const entry = availability[id];
            const unavailable = entry.available ? null : entry;
            return (
              <div className={styles.transportOption} key={id}>
                {/* Not aria-disabled: the button IS operable, and pressing it
                    is the only way to reach the explanation. Declaring it
                    disabled tells a screen reader to skip the one control
                    carrying the reason. The caption is the description
                    instead, which also supersedes a title attribute. */}
                <button
                  type="button"
                  className={`${styles.transportBtn} ${transportType === id ? styles.active : ''} ${unavailable ? styles.unsupportedBtn : ''}`}
                  onClick={() => pickTransport(id)}
                  aria-pressed={transportType === id}
                  aria-describedby={unavailable ? captionId(id) : undefined}
                  title={unavailable ? undefined : title}
                >
                  <Icon size={16} /> {label}
                </button>
                {unavailable && (
                  <span id={captionId(id)} className={styles.unsupportedCaption}>{unavailable.caption}</span>
                )}
              </div>
            );
          })}
        </div>

        <div className={styles.mockDivider}><span>or</span></div>
        <button
          className={`${styles.transportBtn} ${styles.mockBtn} ${transportType === 'websocket' ? styles.active : ''}`}
          onClick={() => pickTransport('websocket')}
          aria-pressed={transportType === 'websocket'}
        >
          <IconMonitor size={16} /> Mock Node (WebSocket)
        </button>

        {/* Bluetooth connection settings */}
        {transportType === 'ble' && !selectedUnavailable && (
          <div className={styles.wifiInput}>
            <div className={styles.field}>
              <label className={styles.wifiLabel}>Device</label>
              {bleDevice ? (
                <div className={styles.tokenRow}>
                  <input
                    type="text"
                    className={styles.wifiField}
                    value={bleDevice.name || bleDevice.id}
                    readOnly
                    aria-label="Selected device"
                  />
                  <button type="button" className={styles.showHideBtn} onClick={handlePickBleDevice}>
                    Change
                  </button>
                </div>
              ) : (
                <button type="button" className={styles.selectDeviceBtn} onClick={handlePickBleDevice}>
                  Select device…
                </button>
              )}
              {blePickError && <span className={styles.wifiHint}>{blePickError}</span>}
              {!bleDevice && (
                <span className={styles.wifiHint}>
                  Scans for nearby Bramble nodes; pick yours, then connect below.
                </span>
              )}
            </div>

            <div className={styles.field}>
              <label htmlFor="ble-token" className={styles.wifiLabel}>Auth Token</label>
              <div className={styles.tokenRow}>
                <input
                  id="ble-token"
                  aria-label="Auth Token"
                  type={showBleToken ? 'text' : 'password'}
                  className={`${styles.wifiField} ${authError ? styles.authErrorField : ''}`}
                  value={bleToken}
                  onChange={e => setBleToken(e.target.value)}
                  onKeyDown={e => e.key === 'Enter' && handleConnect()}
                  autoComplete="off"
                />
                <button
                  type="button"
                  className={styles.showHideBtn}
                  onClick={() => setShowBleToken(v => !v)}
                  aria-label={showBleToken ? 'Hide token' : 'Show token'}
                >
                  {showBleToken ? 'Hide' : 'Show'}
                </button>
              </div>
              <span className={styles.wifiHint}>
                Read it over USB with the bramble CLI (pair command), or from the node's Config page. Leave blank if the node has auth disabled.
              </span>
            </div>

            <div className={styles.field}>
              <label htmlFor="ble-name" className={styles.wifiLabel}>Name (optional)</label>
              <input
                id="ble-name"
                type="text"
                className={styles.wifiField}
                value={bleName}
                onChange={e => setBleName(e.target.value)}
                placeholder="e.g. Living room node"
                onKeyDown={e => e.key === 'Enter' && handleConnect()}
              />
            </div>

            <div className={styles.field}>
              <label className={styles.rememberRow}>
                <input
                  type="checkbox"
                  checked={bleRemember}
                  onChange={e => setBleRemember(e.target.checked)}
                />
                <span>Remember this device</span>
              </label>
              <span className={`${styles.wifiHint} ${styles.rememberHint}`}>
                Saves this node's auth token in this browser so you do not retype it; leave off on shared computers. The device stays in your list either way, and Forget removes it.
              </span>
            </div>
          </div>
        )}

        {/* WiFi connection settings */}
        {transportType === 'wifi' && !selectedUnavailable && (
          <div className={styles.wifiInput}>
            <NearbyNodes onPickUnknown={(n: NearbyNode) => {
              setWifiIp(n.ip);
              setWifiName(n.txtName ?? '');
            }} />
            <div className={styles.field}>
              <label htmlFor="wifi-ip" className={styles.wifiLabel}>Node address</label>
              <input
                id="wifi-ip"
                type="text"
                className={styles.wifiField}
                value={wifiIp}
                onChange={e => setWifiIp(e.target.value)}
                placeholder="192.168.4.1"
                onKeyDown={e => e.key === 'Enter' && handleConnect()}
              />
              <span className={styles.wifiHint}>
                The node's IP, not this web UI. AP mode: 192.168.4.1, Station mode: check your router.
              </span>
            </div>

            <div className={styles.field}>
              <label htmlFor="wifi-token" className={styles.wifiLabel}>Auth Token</label>
              <div className={styles.tokenRow}>
                <input
                  id="wifi-token"
                  aria-label="Auth Token"
                  type={showToken ? 'text' : 'password'}
                  className={`${styles.wifiField} ${authError ? styles.authErrorField : ''}`}
                  value={wifiToken}
                  onChange={e => setWifiToken(e.target.value)}
                  onKeyDown={e => e.key === 'Enter' && handleConnect()}
                  autoComplete="off"
                />
                <button
                  type="button"
                  className={styles.showHideBtn}
                  onClick={() => setShowToken(v => !v)}
                  aria-label={showToken ? 'Hide token' : 'Show token'}
                >
                  {showToken ? 'Hide' : 'Show'}
                </button>
              </div>
              <span className={styles.wifiHint}>
                Read it over USB with the bramble CLI (pair command), or from the node's Config page.
              </span>
            </div>

            <div className={styles.field}>
              <label htmlFor="wifi-name" className={styles.wifiLabel}>Name (optional)</label>
              <input
                id="wifi-name"
                type="text"
                className={styles.wifiField}
                value={wifiName}
                onChange={e => setWifiName(e.target.value)}
                placeholder="e.g. Living room node"
                onKeyDown={e => e.key === 'Enter' && handleConnect()}
              />
            </div>

            <div className={styles.field}>
              <label className={styles.rememberRow}>
                <input
                  type="checkbox"
                  checked={wifiRemember}
                  onChange={e => setWifiRemember(e.target.checked)}
                />
                <span>Remember this device</span>
              </label>
              <span className={`${styles.wifiHint} ${styles.rememberHint}`}>
                Saves this node's auth token in this browser; leave off on shared computers. The device stays in your list either way, and Forget removes it.
              </span>
            </div>
          </div>
        )}

        {selectedUnavailable && <TransportUnavailableNotice info={selectedUnavailable} />}

        {errorFrom === 'form' && pairingBanner}

        {connectionError && errorFrom === 'form' && (
          <div className={styles.error}>
            <span><IconWarning size={14} /> {connectionError}</span>
          </div>
        )}

        {!selectedUnavailable && (
          <>
            <button
              className={styles.connectBtn}
              onClick={handleConnect}
              disabled={isConnecting || (transportType === 'wifi' && !wifiIp.trim())}
            >
              {isConnecting ? (
                <span className={styles.spinner}>
                  {/* aria-hidden: the adjacent label announces the state, and
                      a fixed 'Scanning' label here contradicted it the moment
                      it flipped to Pairing…. */}
                  {transportType === 'ble' && <span className={styles.spinnerIcon} aria-hidden="true" />}
                  {connectingLabelFor(transportType, pairingPending)}
                </span>
              ) : (
                'Connect'
              )}
            </button>

            <p className={styles.hint}>{hints[transportType]}</p>
          </>
        )}

        {/* The flasher is a hosted sibling page (webapp/public/web-flasher/),
            so the relative link only makes sense in a real browser, not in the
            Electron/Android shells. */}
        {!isEmbeddedShell() && (
          <p className={styles.flasherHint}>
            Brand-new device?{' '}
            <a className={styles.flasherLink} href="./web-flasher/">
              Flash Bramble firmware from your browser
            </a>
          </p>
        )}
      </div>
    </div>
  );
}
