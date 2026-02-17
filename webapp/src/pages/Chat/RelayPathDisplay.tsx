import React from 'react';
import type { RelayHop } from '../../types/bramble';
import { AddressLabel } from '../../components/AddressLabel';
import styles from './RelayPathDisplay.module.css';

interface RelayPathDisplayProps {
  path: RelayHop[];   // ordered: [relay1, relay2, …, dest]
  myAddr: number;
  destAddr: number;
}

export function RelayPathDisplay({ path, myAddr, destAddr }: RelayPathDisplayProps) {
  // Build hop list: [me, ...relay hops from path (excluding final dest), dest]
  // path already ends at dest, with rssi = signal at each hop receiver
  const hops: Array<{ addr: number; rssi?: number }> = [
    { addr: myAddr },
    ...path,
  ];

  // Last hop is the destination – relabelfor clarity
  const lastIndex = hops.length - 1;

  return (
    <div className={styles.path} title="Critical message relay path">
      <span className={styles.pathLabel}>via</span>
      {hops.map((hop, i) => (
        <React.Fragment key={`${hop.addr}-${i}`}>
          {i > 0 && (
            <span
              className={styles.arrow}
              title={hop.rssi !== undefined && hop.rssi !== 0 ? `RSSI ${hop.rssi} dBm` : undefined}
            >
              →
            </span>
          )}
          <span className={styles.hopWrap}>
            <AddressLabel
              addr={hop.addr}
              short
              className={i === 0 ? styles.hopSelf : i === lastIndex ? styles.hopDest : styles.hop}
            />
            {hop.rssi !== undefined && hop.rssi !== 0 && (
              <span className={styles.rssi}>{hop.rssi}</span>
            )}
          </span>
        </React.Fragment>
      ))}
    </div>
  );
}
