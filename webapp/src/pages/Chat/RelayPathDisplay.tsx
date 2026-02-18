import React from 'react';
import type { RelayHop } from '../../types/bramble';
import styles from './RelayPathDisplay.module.css';

interface RelayPathDisplayProps {
  path: RelayHop[];
  myAddr: number;
}

function shortAddr(addr: number): string {
  return '0x' + addr.toString(16).toUpperCase().padStart(8, '0').slice(-4);
}

export function RelayPathDisplay({ path, myAddr }: RelayPathDisplayProps) {
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
            <span className={cls}>{shortAddr(hop.addr)}</span>
            {hop.rssi !== 0 && (
              <span className={styles.rssi}>{hop.rssi}</span>
            )}
          </React.Fragment>
        );
      })}
    </div>
  );
}
