import { useEffect, useState } from 'react';
import { useStore } from '../store/index';
import { mergeNearby, type NearbyNode } from '../lib/nearbyNodes';
import type { DiscoveredNode } from '../types/desktop';
import { connectToSavedDevice } from './ConnectionOverlay';
import styles from './DeviceList.module.css';

// Desktop-only: lists nodes found via mDNS on the LAN. Saved nodes (device
// book) one-click connect; unknown nodes prefill the add-device form via
// onPickUnknown. Renders nothing on web, where window.brambleDesktop is
// undefined.
export function NearbyNodes({ onPickUnknown }: { onPickUnknown: (node: NearbyNode) => void }) {
  const devices = useStore(s => s.devices);
  const [discovered, setDiscovered] = useState<DiscoveredNode[]>([]);
  const desktop = window.brambleDesktop;

  useEffect(() => {
    if (!desktop) return;
    const unsubscribe = desktop.onDiscovered(setDiscovered);
    desktop.startDiscovery();
    return () => {
      unsubscribe();
      desktop.stopDiscovery();
    };
  }, [desktop]);

  if (!desktop) return null;
  const nodes = mergeNearby(discovered, devices);
  if (nodes.length === 0) return null;

  const onPick = (n: NearbyNode) => {
    const match = n.saved ?? n.probableSaved;
    if (!match) {
      onPickUnknown(n);
      return;
    }
    // Discovery knows the node's current IP, which may be fresher than the book's lastIp.
    connectToSavedDevice(match, n.ip);
  };

  return (
    <div className={styles.book}>
      <h3 className={styles.heading}>Nearby nodes</h3>
      <ul className={styles.list}>
        {nodes.map(n => (
          <li key={n.key} className={styles.row}>
            <button
              type="button"
              className={styles.connectBtn}
              onClick={() => onPick(n)}
              aria-label={`Connect to ${n.displayName}`}
              title={n.probableSaved ? 'Matched by hostname; the address is verified on connect' : undefined}
            >
              <span className={styles.name}>{n.displayName}</span>
              <span className={styles.right}>
                <span className={styles.meta}>{n.ip}</span>
                <span className={styles.chevron} aria-hidden="true" />
              </span>
            </button>
          </li>
        ))}
      </ul>
    </div>
  );
}
