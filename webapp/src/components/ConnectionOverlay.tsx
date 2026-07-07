import { useEffect, useState } from 'react';
import { connect, refreshDevices } from '../store/actions';
import { useStore } from '../store/index';
import type { TransportType } from '../types/bramble';
import { IconUsb, IconBluetooth, IconMonitor, IconWifi, IconWarning } from './Icons';
import { DeviceList } from './DeviceList';
import styles from './ConnectionOverlay.module.css';

const WIFI_IP_KEY = 'bramble_wifi_ip';
const WIFI_TOKEN_KEY = 'bramble_wifi_token';

export function buildWifiUrl(ip: string, protocol: string, host: string, _token?: string): string {
  let url: string;
  if (ip.includes('://')) url = ip;
  else if (protocol === 'https:') url = `wss://${host}/proxy/${ip}`;
  else url = `ws://${ip}/ws`;

  // Token is NOT embedded in the URL (NEW-SEC-6): it rides the WS subprotocol.
  return url;
}

export function connectingLabelFor(transportType: TransportType): string {
  if (transportType === 'ble') return 'Scanning…';
  if (transportType === 'serial') return 'Opening serial…';
  if (transportType === 'wifi') return 'Handshaking…';
  return 'Connecting…';
}

export function shouldAutoConnect(_savedIp: string, _autoTried: boolean, _connectionState: string, _manualDisconnect: boolean): boolean {
  // Explicit policy: never auto-connect. User must press Connect.
  return false;
}

function loadSavedIp(): string {
  try { return localStorage.getItem(WIFI_IP_KEY) || ''; } catch { return ''; }
}

// Migrate any token left in localStorage from before S19 fix
try {
  const legacyToken = localStorage.getItem(WIFI_TOKEN_KEY);
  if (legacyToken) {
    sessionStorage.setItem(WIFI_TOKEN_KEY, legacyToken);
    localStorage.removeItem(WIFI_TOKEN_KEY);
  }
} catch { /* noop */ }

function loadSavedToken(): string {
  // S19 fix: auth token uses sessionStorage (not localStorage) to limit
  // persistence window and reduce XSS exfiltration risk
  try { return sessionStorage.getItem(WIFI_TOKEN_KEY) || ''; } catch { return ''; }
}

function saveWifiSettings(ip: string, token: string) {
  try {
    localStorage.setItem(WIFI_IP_KEY, ip); // IP is non-sensitive, keep persistent
    sessionStorage.setItem(WIFI_TOKEN_KEY, token); // Token: session-only
  } catch {
    /* noop */
  }
}

export function ConnectionOverlay() {
  const savedIp = loadSavedIp();
  const savedToken = loadSavedToken();
  const [transportType, setTransportType] = useState<TransportType>(savedIp ? 'wifi' : 'serial');
  const [wifiIp, setWifiIp] = useState(savedIp);
  const [wifiToken, setWifiToken] = useState(savedToken);
  const [wifiRemember, setWifiRemember] = useState(false);
  const [wifiName, setWifiName] = useState('');
  const [showToken, setShowToken] = useState(false);
  const [autoTried, setAutoTried] = useState(false);
  const connectionState = useStore(s => s.connectionState);
  const connectionError = useStore(s => s.connectionError);
  const manualDisconnect = useStore(s => s.manualDisconnect);
  const connectionCapabilities = useStore(s => s.connectionCapabilities);

  const isConnecting = connectionState === 'connecting';
  const authError = /1008|unauthorized|auth/i.test(connectionError ?? '');

  useEffect(() => {
    refreshDevices();
  }, []);

  const handleConnect = () => {
    if (transportType === 'wifi') {
      const ip = wifiIp.trim();
      const token = wifiToken.trim();
      if (!ip) return;
      // Do NOT call the legacy saveWifiSettings here: it wrote the token to the
      // legacy sessionStorage key that the device book now supersedes, recreating
      // a key forgetDevice() deletes. The book saves lastIp per device on connect
      // (Task 3). Persist only the IP for the empty-form prefill convenience.
      try { localStorage.setItem('bramble_wifi_ip', ip); } catch { /* noop */ }
      const url = buildWifiUrl(ip, location.protocol, location.host, token || undefined);
      connect(transportType, { url, token: token || undefined, ip, remember: wifiRemember, name: wifiName.trim() || undefined });
    } else {
      connect(transportType);
    }
  };

  useEffect(() => {
    if (!shouldAutoConnect(savedIp, autoTried, connectionState, manualDisconnect)) return;
    setAutoTried(true);
    const token = savedToken.trim();
    const url = buildWifiUrl(savedIp, location.protocol, location.host, token || undefined);
    connect('wifi', { url, token: token || undefined });
  }, [autoTried, savedIp, savedToken, connectionState, manualDisconnect]);

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
    wifi: 'Enter the IP address of your Bramble node. The node must be on the same network (Station mode) or you must be connected to its hotspot (AP mode).',
  };

  return (
    <div className={styles.overlay}>
      <div className={styles.card}>
        <img src="/bramble-logo.png" alt="Bramble" className={styles.logoImg} />
        <h1 className={styles.title}>Bramble</h1>
        <p className={styles.subtitle}>LoRa mesh companion</p>
        <span className={styles.version}>{__APP_VERSION__}</span>
        <span className={styles.runtimeBadge} title={`Runtime context: ${runtimeBadge}`}>{runtimeBadge}</span>

        <DeviceList />

        <div className={styles.transportSelect}>
          <div className={styles.transportOption}>
            <button
              className={`${styles.transportBtn} ${transportType === 'serial' ? styles.active : ''} ${!hasSerial ? styles.unsupportedBtn : ''}`}
              onClick={() => hasSerial && setTransportType('serial')}
              disabled={!hasSerial}
              title={hasSerial ? 'Connect via USB cable' : 'Web Serial not supported in this browser. Use Chrome or Edge 120+.'}
            >
              <IconUsb size={16} /> USB / Serial
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

        <div className={styles.mockDivider}>— or —</div>
        <button
          className={`${styles.transportBtn} ${styles.mockBtn} ${transportType === 'websocket' ? styles.active : ''}`}
          onClick={() => setTransportType('websocket')}
        >
          <IconMonitor size={16} /> Mock Node (WebSocket)
        </button>

        {/* WiFi connection settings */}
        {transportType === 'wifi' && (
          <div className={styles.wifiInput}>
            <label htmlFor="wifi-ip" className={styles.wifiLabel}>Node address (not web UI)</label>
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
              AP mode: 192.168.4.1 · Station mode: check your router
            </span>

            <div className={styles.authPanel}>
              <label htmlFor="wifi-token" className={styles.authLabel}>Auth Token</label>
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
                Auth token: read it over USB with the bramble CLI (pair command), or from the node's Config page.
              </span>
            </div>

            <label className={styles.rememberRow}>
              <input
                type="checkbox"
                checked={wifiRemember}
                onChange={e => setWifiRemember(e.target.checked)}
              />
              <span>Remember this device</span>
            </label>
            <p className={styles.hint}>Remembered tokens are stored in this browser; leave off for shared/public computers.</p>

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
