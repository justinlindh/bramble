import type { NodeStatus, BrambleConfig } from '../../types/bramble';
import styles from './SystemInfo.module.css';

function formatUptime(seconds: number): string {
  const d = Math.floor(seconds / 86_400);
  const h = Math.floor((seconds % 86_400) / 3_600);
  const m = Math.floor((seconds % 3_600) / 60);
  const s = seconds % 60;
  if (d > 0) return `${d}d ${h}h ${m}m`;
  if (h > 0) return `${h}h ${m}m`;
  return `${m}m ${s}s`;
}

function formatBytes(bytes: number): string {
  if (bytes >= 1_048_576) return `${(bytes / 1_048_576).toFixed(1)} MB`;
  if (bytes >= 1_024) return `${(bytes / 1_024).toFixed(1)} KB`;
  return `${bytes} B`;
}

function formatAddr(addr: number): string {
  return `0x${addr.toString(16).toUpperCase().padStart(8, '0')}`;
}

interface Row {
  label: string;
  value: string;
  mono?: boolean;
  color?: 'muted' | 'warning' | 'danger';
}

interface Props {
  status: NodeStatus;
  config: BrambleConfig;
}

function hasBattery(status: NodeStatus): boolean {
  // Boards without a battery commonly report 0mV / 0%.
  if (status.batteryMv === undefined && status.batteryPct === undefined) return false;
  return (status.batteryMv ?? 0) > 1000;
}

export function SystemInfo({ status, config }: Props) {
  const { identity } = config;

  // Heap health indicator
  const heapWarning = status.freeHeapBytes < 20_000;
  const heapDanger  = status.freeHeapBytes < 8_000;

  const rows: Row[] = [
    {
      label: 'Uptime',
      value: formatUptime(status.uptimeSec),
    },
    {
      label: 'Free Heap',
      value: formatBytes(status.freeHeapBytes),
      mono: true,
      color: heapDanger ? 'danger' : heapWarning ? 'warning' : undefined,
    },
    ...((status.batteryPct !== undefined || status.batteryMv !== undefined) ? [{
      label: 'Battery',
      value: hasBattery(status) ? `${status.batteryPct ?? '?'}% (${status.batteryMv ?? '?'} mV)` : 'N/A',
      color: hasBattery(status)
        ? (status.batteryPct ?? 100) < 10
          ? 'danger' as const
          : (status.batteryPct ?? 100) < 25
            ? 'warning' as const
            : undefined
        : 'muted' as const,
    }] : []),
    {
      label: 'Firmware',
      value: status.fwVersion,
      mono: true,
    },
    {
      label: 'Node Name',
      value: identity.name,
    },
    {
      label: 'Address',
      value: formatAddr(identity.address),
      mono: true,
    },
    {
      label: 'Pubkey Hash',
      value: formatAddr(identity.pubkeyHash),
      mono: true,
    },
  ];

  return (
    <section className={styles.card}>
      <h2 className={styles.heading}>ℹ️ System Info</h2>
      <dl className={styles.list}>
        {rows.map(({ label, value, mono, color }) => (
          <div key={label} className={styles.row}>
            <dt className={styles.dt}>{label}</dt>
            <dd
              className={[
                styles.dd,
                mono ? styles.mono : '',
                color ? styles[color] : '',
              ]
                .filter(Boolean)
                .join(' ')}
            >
              {value}
            </dd>
          </div>
        ))}
      </dl>
    </section>
  );
}
