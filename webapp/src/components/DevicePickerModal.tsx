import { useEffect, useState } from 'react';
import type { DevicePickerRequest } from '../types/desktop';
import { EscapeDialog } from './EscapeDialog';
import styles from './DevicePickerModal.module.css';

/**
 * Electron-only chooser for Web Serial / Web Bluetooth requests. Electron
 * has no built-in picker UI: the main process forwards the candidate list
 * over IPC and this modal lets the user pick the actual device instead of
 * silently auto-selecting one (which connected to the wrong node when two
 * were attached). For Bluetooth the list refreshes live while scanning.
 */
export function DevicePickerModal() {
  const [request, setRequest] = useState<DevicePickerRequest>(null);

  useEffect(() => {
    const api = window.brambleDesktop;
    if (!api?.onDevicePicker) return;
    return api.onDevicePicker(setRequest);
  }, []);

  if (!request) return null;

  const title = request.kind === 'serial' ? 'Select a serial device' : 'Select a Bluetooth device';
  const scanning = request.kind === 'bluetooth';

  return (
    <EscapeDialog
      ariaLabel={title}
      onClose={() => window.brambleDesktop?.cancelDevicePicker()}
      backdropClassName={styles.backdrop}
      dialogClassName={styles.modal}
    >
      <div className={styles.title}>{title}</div>
      {scanning && <div className={styles.hint}>Scanning for nearby Bramble nodes…</div>}
      <div className={styles.list}>
        {request.devices.length === 0 && (
          <div className={styles.empty}>No devices found yet</div>
        )}
        {request.devices.map(d => (
          <button
            key={d.id}
            className={styles.device}
            onClick={() => window.brambleDesktop?.selectDevice(d.id)}
          >
            <span className={styles.deviceLabel}>{d.label}</span>
            {d.detail && <span className={styles.deviceDetail}>{d.detail}</span>}
          </button>
        ))}
      </div>
      <button
        className={styles.cancelBtn}
        onClick={() => window.brambleDesktop?.cancelDevicePicker()}
      >
        Cancel
      </button>
    </EscapeDialog>
  );
}
