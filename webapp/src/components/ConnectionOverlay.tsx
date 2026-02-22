import { useState } from 'react';
import { connect } from '../store/actions';
import { useStore } from '../store/index';
import type { TransportType } from '../types/bramble';
import { IconUsb, IconBluetooth, IconMonitor, IconWifi, IconWarning } from './Icons';
import styles from './ConnectionOverlay.module.css';

const WIFI_IP_KEY = 'bramble_wifi_ip';

export function buildWifiUrl(ip: string, protocol: string, host: string): string {
  if (ip.includes('://')) return ip;
  if (protocol === 'https:') return `wss://${host}/proxy/${ip}`;
  return `ws://${ip}/ws`;
}

export function connectingLabelFor(transportType: TransportType): string {
  if (transportType === 'ble') return 'Scanning…';
  if (transportType === 'serial') return 'Opening serial…';
  if (transportType === 'wifi') return 'Handshaking…';
  return 'Connecting…';
}

function loadSavedIp(): string {
  try { return localStorage.getItem(WIFI_IP_KEY) || ''; } catch { return ''; }
}

function saveIp(ip: string) {
  try { localStorage.setItem(WIFI_IP_KEY, ip); } catch { /* noop */ }
}

export function ConnectionOverlay() {
  const [transportType, setTransportType] = useState<TransportType>('serial');
  const [wifiIp, setWifiIp] = useState(loadSavedIp);
  const connectionState = useStore(s => s.connectionState);
  const connectionError = useStore(s => s.connectionError);

  const isConnecting = connectionState === 'connecting';

  const handleConnect = () => {
    if (transportType === 'wifi') {
      const ip = wifiIp.trim();
      if (!ip) return;
      saveIp(ip);
      const url = buildWifiUrl(ip, location.protocol, location.host);
      connect(transportType, { url });
    } else {
      connect(transportType);
    }
  };

  // Check browser support
  const hasSerial = 'serial' in navigator;
  const hasBluetooth = 'bluetooth' in navigator;

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

        <div className={styles.transportSelect}>
          <button
            className={`${styles.transportBtn} ${transportType === 'serial' ? styles.active : ''} ${!hasSerial ? styles.unsupportedBtn : ''}`}
            onClick={() => hasSerial && setTransportType('serial')}
            disabled={!hasSerial}
            title={hasSerial ? 'Connect via USB cable' : 'Web Serial not supported in this browser. Use Chrome or Edge 120+.'}
          >
            <IconUsb size={16} /> USB / Serial
          </button>
          <button
            className={`${styles.transportBtn} ${transportType === 'ble' ? styles.active : ''} ${!hasBluetooth ? styles.unsupportedBtn : ''}`}
            onClick={() => hasBluetooth && setTransportType('ble')}
            disabled={!hasBluetooth}
            title={hasBluetooth ? 'Connect via Bluetooth Low Energy' : 'Web Bluetooth not supported in this browser. Use Chrome on Android, or enable Experimental Web Platform features in chrome://flags.'}
          >
            <IconBluetooth size={16} /> Bluetooth
          </button>
          <button
            className={`${styles.transportBtn} ${transportType === 'wifi' ? styles.active : ''}`}
            onClick={() => setTransportType('wifi')}
          >
            <IconWifi size={16} /> WiFi
          </button>
        </div>

        <div className={styles.mockDivider}>— or —</div>
        <button
          className={`${styles.transportBtn} ${styles.mockBtn} ${transportType === 'websocket' ? styles.active : ''}`}
          onClick={() => setTransportType('websocket')}
        >
          <IconMonitor size={16} /> Mock Node (WebSocket)
        </button>

        {/* WiFi IP input */}
        {transportType === 'wifi' && (
          <div className={styles.wifiInput}>
            <label className={styles.wifiLabel}>Node IP address</label>
            <input
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
            <span className={styles.spinner}>{connectingLabelFor(transportType)}</span>
          ) : (
            'Connect'
          )}
        </button>

        <p className={styles.hint}>{hints[transportType]}</p>
      </div>
    </div>
  );
}
