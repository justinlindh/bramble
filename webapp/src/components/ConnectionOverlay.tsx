import { useState } from 'react';
import { connect } from '../store/actions';
import { useStore } from '../store/index';
import type { TransportType } from '../types/bramble';
import styles from './ConnectionOverlay.module.css';

export function ConnectionOverlay() {
  const [transportType, setTransportType] = useState<TransportType>('serial');
  const connectionState = useStore(s => s.connectionState);
  const connectionError = useStore(s => s.connectionError);

  const isConnecting = connectionState === 'connecting';

  const handleConnect = () => {
    connect(transportType);
  };

  // Check browser support
  const hasSerial = 'serial' in navigator;
  const hasBluetooth = 'bluetooth' in navigator;
  const hasAnyHardwareSupport = hasSerial || hasBluetooth;

  // Show WebSocket mock when in dev mode or when no hardware APIs available
  const isDev = import.meta.env.DEV;
  const showMock = isDev || !hasAnyHardwareSupport;

  return (
    <div className={styles.overlay}>
      <div className={styles.card}>
        <div className={styles.logo}>🌿</div>
        <h1 className={styles.title}>Bramble</h1>
        <p className={styles.subtitle}>LoRa mesh companion</p>

        {!hasAnyHardwareSupport && !isDev && (
          <div className={styles.unsupported}>
            <p>⚠️ Your browser does not support Web Serial or Web Bluetooth.</p>
            <p className={styles.hint}>
              Use Chrome or Edge 120+ on desktop, or Chrome on Android for BLE.
            </p>
          </div>
        )}

        {hasAnyHardwareSupport && (
          <>
            <div className={styles.transportSelect}>
              <button
                className={`${styles.transportBtn} ${transportType === 'serial' ? styles.active : ''}`}
                onClick={() => setTransportType('serial')}
                disabled={!hasSerial}
              >
                🔌 USB / Serial
              </button>
              <button
                className={`${styles.transportBtn} ${transportType === 'ble' ? styles.active : ''}`}
                onClick={() => setTransportType('ble')}
                disabled={!hasBluetooth}
              >
                📡 Bluetooth
              </button>
            </div>
          </>
        )}

        {showMock && (
          <>
            {hasAnyHardwareSupport && (
              <div className={styles.mockDivider}>— or —</div>
            )}
            <button
              className={`${styles.transportBtn} ${styles.mockBtn} ${transportType === 'websocket' ? styles.active : ''}`}
              onClick={() => setTransportType('websocket')}
            >
              🖥️ Mock Node (WebSocket)
            </button>
          </>
        )}

        {connectionError && (
          <div className={styles.error}>
            <span>⚠️ {connectionError}</span>
          </div>
        )}

        <button
          className={styles.connectBtn}
          onClick={handleConnect}
          disabled={isConnecting}
        >
          {isConnecting ? (
            <span className={styles.spinner}>Connecting…</span>
          ) : (
            'Connect'
          )}
        </button>

        <p className={styles.hint}>
          {transportType === 'serial'
            ? 'Connect your Bramble node via USB cable, then click Connect.'
            : transportType === 'ble'
            ? 'Enable Bluetooth on your device, then click Connect to scan.'
            : 'Connects to the local mock node server at ws://localhost:3005.'}
        </p>
      </div>
    </div>
  );
}
