import { useRef, useState } from 'react';
import { useStore } from '../store/index';
import { forgetSavedDevice, renameSavedDevice } from '../store/actions';
import { useTimedFlag } from '../hooks/useTimedFlag';
import type { SavedDevice } from '../lib/deviceBook';
import styles from './DeviceList.module.css';

// How long an armed Forget stays armed. Long enough to move the pointer back
// to the same button, short enough that a forgotten first click cannot turn a
// much later stray click into a silent delete.
const FORGET_CONFIRM_MS = 4000;

type DeviceListProps = {
  /** Row click handler: transport selection, prefill, and connect belong to
      the parent (ConnectionOverlay), which owns the form the row prefills. */
  onConnect: (d: SavedDevice) => void;
  /** Address of the row whose connect is in flight, for the busy spinner. */
  busyAddress: string | null;
  /** True while any connect is in flight. Everything locks: a second click
      used to tear down the first attempt mid-flight and start overlapping
      pairing sequences. */
  disabled: boolean;
};

export function DeviceList({ onConnect, busyAddress, disabled }: DeviceListProps) {
  const devices = useStore(s => s.devices);
  const [renaming, setRenaming] = useState<string | null>(null);
  const [draftName, setDraftName] = useState('');
  const [confirmingForget, setConfirmingForget] = useState<string | null>(null);
  // useTimedFlag owns the arm window's timer lifecycle (restart on
  // re-trigger, cancel on unmount); a row is armed when the flag is up AND
  // confirmingForget names it.
  const [forgetArmed, armForget, disarmForget] = useTimedFlag(FORGET_CONFIRM_MS);
  const headingRef = useRef<HTMLHeadingElement | null>(null);
  if (devices.length === 0) return null;

  // Closing the rename form unmounts the focused input, which would drop a
  // keyboard user's position to <body>: put focus back on the row's Rename
  // button once it has re-rendered.
  const closeRename = (address: string) => {
    setRenaming(null);
    setTimeout(() => {
      (document.querySelector(`[data-rename-btn="${address}"]`) as HTMLElement | null)?.focus();
    }, 0);
  };

  // Two-step inline confirm instead of window.confirm: the first click arms
  // this row's Forget, the second within the window executes, and the flag's
  // timer disarms it so the armed state cannot lie in wait indefinitely.
  const isArmed = (address: string) => forgetArmed && confirmingForget === address;
  const handleForget = (d: SavedDevice) => {
    if (isArmed(d.address)) {
      disarmForget();
      setConfirmingForget(null);
      forgetSavedDevice(d.address);
      // The focused button vanished with its row: land on the list heading
      // (when devices remain) instead of <body>.
      setTimeout(() => headingRef.current?.focus(), 0);
      return;
    }
    setConfirmingForget(d.address);
    armForget();
  };

  return (
    <div className={styles.book}>
      {/* tabIndex -1: programmatic focus target after a row is forgotten. */}
      <h3 className={styles.heading} ref={headingRef} tabIndex={-1}>Your devices</h3>
      <ul className={styles.list}>
        {devices.map(d => (
          <li key={d.address} className={styles.row}>
            {renaming === d.address ? (
              <form
                className={styles.renameForm}
                onSubmit={e => { e.preventDefault(); renameSavedDevice(d.address, draftName.trim() || d.name); closeRename(d.address); }}
                // On the form, not the input, so Escape also works after
                // tabbing to Cancel.
                onKeyDown={e => { if (e.key === 'Escape') { e.preventDefault(); closeRename(d.address); } }}
              >
                <input
                  className={styles.renameInput}
                  autoFocus
                  value={draftName}
                  onChange={e => setDraftName(e.target.value)}
                  aria-label={`Rename ${d.name}`}
                />
                <button
                  type="button"
                  className={styles.action}
                  onClick={() => closeRename(d.address)}
                >
                  Cancel
                </button>
              </form>
            ) : (
              <button
                type="button"
                className={styles.connectBtn}
                onClick={() => onConnect(d)}
                disabled={disabled}
                aria-busy={busyAddress === d.address}
                aria-label={`Connect to ${d.name}`}
              >
                <span className={styles.name}>{d.name}</span>
                <span className={styles.right}>
                  <span className={styles.meta}>{d.transport === 'wifi' ? d.lastIp : d.transport === 'ble' ? 'Bluetooth' : 'USB'}</span>
                  {busyAddress === d.address ? (
                    <span className={styles.spinnerIcon} aria-hidden="true" />
                  ) : (
                    <span className={styles.chevron} aria-hidden="true" />
                  )}
                </span>
              </button>
            )}
            <button
              type="button"
              className={styles.action}
              onClick={() => { setRenaming(d.address); setDraftName(d.name); }}
              disabled={disabled}
              data-rename-btn={d.address}
              aria-label={`Rename ${d.name}`}
            >
              Rename
            </button>
            <button
              type="button"
              className={`${styles.action} ${styles.danger} ${isArmed(d.address) ? styles.armed : ''}`}
              onClick={() => handleForget(d)}
              disabled={disabled}
              aria-label={isArmed(d.address) ? `Confirm forget ${d.name}` : `Forget ${d.name}`}
            >
              {isArmed(d.address) ? 'Forget?' : 'Forget'}
            </button>
          </li>
        ))}
      </ul>
    </div>
  );
}
