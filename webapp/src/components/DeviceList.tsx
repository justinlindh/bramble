import { useState } from 'react';
import { useStore } from '../store/index';
import { connect, forgetSavedDevice, renameSavedDevice } from '../store/actions';
import { getDeviceToken } from '../lib/deviceBook';
import { buildWifiUrl } from './ConnectionOverlay';
import styles from './DeviceList.module.css';

export function DeviceList() {
  const devices = useStore(s => s.devices);
  const [renaming, setRenaming] = useState<string | null>(null);
  const [draftName, setDraftName] = useState('');
  if (devices.length === 0) return null;

  const onConnect = (address: string) => {
    const d = devices.find(x => x.address === address);
    if (!d || d.transport !== 'wifi' || !d.lastIp) return;
    const tok = getDeviceToken(d.address);
    const url = buildWifiUrl(d.lastIp, location.protocol, location.host, tok || undefined);
    connect('wifi', { url, token: tok || undefined, ip: d.lastIp, remember: d.remember, name: d.name, expectAddressHex: d.address });
  };

  return (
    <div className={styles.book}>
      <h3 className={styles.heading}>Your devices</h3>
      <ul className={styles.list}>
        {devices.map(d => (
          <li key={d.address} className={styles.row}>
            {renaming === d.address ? (
              <form
                className={styles.renameForm}
                onSubmit={e => { e.preventDefault(); renameSavedDevice(d.address, draftName.trim() || d.name); setRenaming(null); }}
              >
                <input
                  className={styles.renameInput}
                  autoFocus
                  value={draftName}
                  onChange={e => setDraftName(e.target.value)}
                  aria-label={`Rename ${d.name}`}
                />
              </form>
            ) : (
              <button
                type="button"
                className={styles.connectBtn}
                onClick={() => onConnect(d.address)}
                aria-label={`Connect to ${d.name}`}
              >
                <span className={styles.name}>{d.name}</span>
                <span className={styles.meta}>{d.transport === 'wifi' ? d.lastIp : 'USB'}</span>
              </button>
            )}
            <button
              type="button"
              className={styles.action}
              onClick={() => { setRenaming(d.address); setDraftName(d.name); }}
              aria-label={`Rename ${d.name}`}
            >
              Rename
            </button>
            <button
              type="button"
              className={styles.action}
              onClick={() => forgetSavedDevice(d.address)}
              aria-label={`Forget ${d.name}`}
            >
              Forget
            </button>
          </li>
        ))}
      </ul>
    </div>
  );
}
