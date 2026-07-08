import { useEffect, useState } from 'react';
import { connect, refreshDevices } from '../store/actions';
import { BLETransport } from '../transport/BLETransport';
import { useStore } from '../store/index';
import { getDeviceToken, type SavedDevice } from '../lib/deviceBook';
import { isEmbeddedShell } from '../utils/platform';
import type { TransportType } from '../types/bramble';
import { IconUsb, IconBluetooth, IconMonitor, IconWifi, IconWarning } from './Icons';
import { DeviceList } from './DeviceList';
import { NearbyNodes } from './NearbyNodes';
import type { NearbyNode } from '../lib/nearbyNodes';
import styles from './ConnectionOverlay.module.css';

const WIFI_IP_KEY = 'bramble_wifi_ip';

export function buildWifiUrl(ip: string, protocol: string, host: string, _token?: string): string {
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
export function connectToSavedDevice(d: SavedDevice, ip: string): void {
  const tok = getDeviceToken(d.address);
  const url = buildWifiUrl(ip, location.protocol, location.host, tok || undefined);
  connect('wifi', {
    url,
    token: tok || undefined,
    ip,
    remember: d.remember,
    name: d.name,
    expectAddressHex: d.address,
  });
}

// Reconnect to a saved Bluetooth device with zero prompts: reuse the stored
// token AND the stored BLE identity. pickDevice(expected) resolves silently
// (desktop: main auto-selects the matching candidate; Android: direct connect
// to the stored MAC) and falls back to the chooser when the identity is
// missing or stale. expectAddressHex still verifies the node is the one the
// user chose, dropping the session on a mismatch.
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

export function connectingLabelFor(transportType: TransportType): string {
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

  const isConnecting = connectionState === 'connecting';
  const authError = /1008|unauthorized|auth/i.test(connectionError ?? '');

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
    if (saved) setBleRemember(true);
  }, [devices]);

  const [bleDevice, setBleDevice] = useState<BluetoothDevice | null>(null);
  const [blePickError, setBlePickError] = useState<string | null>(null);

  const handlePickBleDevice = async () => {
    setBlePickError(null);
    try {
      const device = await BLETransport.pickDevice();
      setBleDevice(device);
      // Prefill token/name when this BLE identity is already in the book.
      const known = devices.find(d => d.bleDeviceId === device.id || (device.name && d.bleDeviceName === device.name));
      if (known) {
        const saved = getDeviceToken(known.address);
        if (saved) { setBleToken(saved); setBleRemember(true); }
        setBleName(prev => (prev ? prev : known.name));
      } else if (device.name) {
        setBleName(prev => (prev ? prev : device.name!));
      }
    } catch (e) {
      const msg = (e as Error)?.message ?? '';
      // Cancelling the chooser is not an error state.
      if (!/cancel/i.test(msg)) setBlePickError(msg);
    }
  };

  const handleConnect = () => {
    if (transportType === 'wifi') {
      const ip = wifiIp.trim();
      const token = wifiToken.trim();
      if (!ip) return;
      // The device book owns per-device tokens (saved by address post-connect);
      // here we only persist the IP so the empty form prefills it next time.
      saveLastIp(ip);
      const url = buildWifiUrl(ip, location.protocol, location.host, token || undefined);
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

  // Check browser support
  const hasSerial = 'serial' in navigator;
  const hasBluetooth = 'bluetooth' in navigator;
  const wifiAllowed = connectionCapabilities.localLanAllowed;
  const wifiReason = connectionCapabilities.localLanReason || 'LAN direct connect is unavailable in hosted mode. Use USB or Bluetooth.';
  const runtimeBadge = connectionCapabilities.mode === 'local' ? 'Local LAN' : 'Hosted';

  useEffect(() => {
    if (!wifiAllowed && transportType === 'wifi') {
      setTransportType(hasSerial ? 'serial' : hasBluetooth ? 'ble' : 'websocket');
    }
  }, [wifiAllowed, transportType, hasSerial, hasBluetooth]);

  const hints: Record<TransportType, string> = {
    serial: 'Connect your Bramble node via USB cable, then click Connect.',
    ble: 'Enable Bluetooth on your device, then click Connect to scan.',
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

        <DeviceList />

        {devices.length > 0 && (
          <h3 className={styles.sectionHeading}>Add a device</h3>
        )}

        <div className={styles.transportSelect}>
          <div className={styles.transportOption}>
            <button
              className={`${styles.transportBtn} ${transportType === 'serial' ? styles.active : ''} ${!hasSerial ? styles.unsupportedBtn : ''}`}
              onClick={() => hasSerial && setTransportType('serial')}
              disabled={!hasSerial}
              title={hasSerial ? 'Connect via USB cable' : 'Web Serial not supported in this browser. Use Chrome or Edge 120+.'}
            >
              <IconUsb size={16} /> USB
            </button>
            {!hasSerial && (
              <span className={styles.unsupportedCaption}>Not supported in this browser</span>
            )}
          </div>
          <div className={styles.transportOption}>
            <button
              className={`${styles.transportBtn} ${transportType === 'ble' ? styles.active : ''} ${!hasBluetooth ? styles.unsupportedBtn : ''}`}
              onClick={() => hasBluetooth && setTransportType('ble')}
              disabled={!hasBluetooth}
              title={hasBluetooth ? 'Connect via Bluetooth Low Energy' : 'Web Bluetooth not supported in this browser. Use Chrome on Android, or enable Experimental Web Platform features in chrome://flags.'}
            >
              <IconBluetooth size={16} /> Bluetooth
            </button>
            {!hasBluetooth && (
              <span className={styles.unsupportedCaption}>Not supported in this browser</span>
            )}
          </div>
          <div className={styles.transportOption}>
            <button
              className={`${styles.transportBtn} ${transportType === 'wifi' ? styles.active : ''} ${!wifiAllowed ? styles.unsupportedBtn : ''}`}
              onClick={() => wifiAllowed && setTransportType('wifi')}
              disabled={!wifiAllowed}
              title={wifiAllowed ? 'Connect directly to your Bramble node over LAN WiFi' : wifiReason}
            >
              <IconWifi size={16} /> WiFi
            </button>
            {!wifiAllowed && (
              <span className={styles.unsupportedCaption}>Unavailable in hosted mode</span>
            )}
          </div>
        </div>

        <div className={styles.mockDivider}><span>or</span></div>
        <button
          className={`${styles.transportBtn} ${styles.mockBtn} ${transportType === 'websocket' ? styles.active : ''}`}
          onClick={() => setTransportType('websocket')}
        >
          <IconMonitor size={16} /> Mock Node (WebSocket)
        </button>

        {/* Bluetooth connection settings */}
        {transportType === 'ble' && (
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
                <button type="button" className={styles.showHideBtn} onClick={handlePickBleDevice}>
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
                placeholder="e.g. Node A node"
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
                Stores the token in this browser so you do not retype it; leave off on shared devices.
              </span>
            </div>
          </div>
        )}

        {/* WiFi connection settings */}
        {transportType === 'wifi' && (
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
                placeholder="e.g. Node A node"
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
                Stores the token in this browser; leave off on shared computers.
              </span>
            </div>
          </div>
        )}

        {connectionError && (
          <div className={styles.error}>
            <span><IconWarning size={14} /> {connectionError}</span>
          </div>
        )}

        <button
          className={styles.connectBtn}
          onClick={handleConnect}
          disabled={isConnecting || (transportType === 'wifi' && !wifiIp.trim())}
        >
          {isConnecting ? (
            <span className={styles.spinner}>
              {transportType === 'ble' && <span className={styles.spinnerIcon} aria-label="Scanning in progress" />}
              {connectingLabelFor(transportType)}
            </span>
          ) : (
            'Connect'
          )}
        </button>

        <p className={styles.hint}>{hints[transportType]}</p>
      </div>
    </div>
  );
}
