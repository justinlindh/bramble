import type { RelayHop } from '../../types/bramble';
import styles from './RelayPathDisplay.module.css';

interface RelayPathDisplayProps {
  path: RelayHop[];
  myAddr: number;
}

function shortAddr(addr: number): string {
  return '0x' + addr.toString(16).toUpperCase().padStart(8, '0').slice(-4);
}

function rssiLabel(rssi: number): string {
  if (rssi >= -70) return 'strong';
  if (rssi >= -90) return 'good';
  if (rssi >= -110) return 'weak';
  return 'marginal';
}

export function RelayPathDisplay({ path, myAddr }: RelayPathDisplayProps) {
  if (path.length === 0) return null;

  const hops = [{ addr: myAddr, rssi: 0 }, ...path];

  return (
    <div className={styles.container} title="Delivery path — each node that relayed this message">
      <div className={styles.header}>Relay Path</div>
      <div className={styles.hops}>
        {hops.map((hop, i) => {
          const isFirst = i === 0;
          const isLast = i === hops.length - 1;
          const label = isFirst ? 'You' : isLast ? 'Dest' : `Hop ${i}`;

          return (
            <div key={`${hop.addr}-${i}`} className={styles.hopRow}>
              {/* Connector line */}
              {!isFirst && (
                <div className={styles.connector}>
                  <div className={styles.line} />
                  {hop.rssi !== 0 && (
                    <span className={`${styles.rssi} ${styles[rssiLabel(hop.rssi)]}`}>
                      {hop.rssi} dBm
                    </span>
                  )}
                </div>
              )}
              <div className={`${styles.node} ${isFirst ? styles.self : isLast ? styles.dest : ''}`}>
                <span className={styles.dot} />
                <span className={styles.addr}>{shortAddr(hop.addr)}</span>
                <span className={styles.label}>{label}</span>
              </div>
            </div>
          );
        })}
      </div>
    </div>
  );
}
