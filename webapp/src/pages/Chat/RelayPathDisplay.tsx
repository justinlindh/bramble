import React from 'react';
import type { RelayHop } from '../../types/bramble';
import { useStore } from '../../store/index';
import { formatAddr0x, formatAddrShort } from '../../utils/address';
import styles from './RelayPathDisplay.module.css';

interface RelayPathDisplayProps {
  path: RelayHop[];
}

function rssiQualityClass(rssi: number): string {
  if (rssi > -70) return styles.rssiGood;
  if (rssi >= -90) return styles.rssiFair;
  return styles.rssiPoor;
}

export function RelayPathDisplay({ path }: RelayPathDisplayProps) {
  const peerNames = useStore(s => s.peerNames);
  const myAddr = useStore(s => s.config?.identity.address);
  if (path.length === 0) return null;

  /* path already includes self as first hop from firmware */
  const hops = path;
  const lastIndex = hops.length - 1;

  return (
    <div className={styles.path} title="Message relay path">
      <span className={styles.pathLabel}>VIA</span>
      {hops.map((hop, i) => {
        const isSelf = hop.addr === myAddr;
        const cls = isSelf ? styles.hopSelf : i === lastIndex ? styles.hopDest : styles.hop;
        return (
          <React.Fragment key={`${hop.addr}-${i}`}>
            {i > 0 && <span className={styles.arrow}>→</span>}
            <span className={cls} title={formatAddr0x(hop.addr)}>
              {peerNames.get(hop.addr) || formatAddrShort(hop.addr)}
            </span>
            {hop.rssi !== 0 && (
              <span className={`${styles.rssi} ${rssiQualityClass(hop.rssi)}`}>{hop.rssi} dBm</span>
            )}
          </React.Fragment>
        );
      })}
    </div>
  );
}
