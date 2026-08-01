import { useEffect, useRef, useState } from 'react';
import type { NodeStatus, BrambleConfig } from '../../types/bramble';
import { formatAddr0x } from '../../utils/address';
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

async function copyWithFallback(text: string): Promise<boolean> {
  try {
    if (navigator.clipboard?.writeText) {
      await navigator.clipboard.writeText(text);
      return true;
    }
  } catch {
    // Fall back to legacy copy path below.
  }

  try {
    const textarea = document.createElement('textarea');
    textarea.value = text;
    textarea.setAttribute('readonly', '');
    textarea.style.position = 'fixed';
    textarea.style.opacity = '0';
    textarea.style.left = '-9999px';
    document.body.appendChild(textarea);
    textarea.select();

    const copied = typeof document.execCommand === 'function' && document.execCommand('copy');
    document.body.removeChild(textarea);
    return copied;
  } catch {
    return false;
  }
}

interface Row {
  label: string;
  value: string;
  mono?: boolean;
  color?: 'muted' | 'warning' | 'danger';
  copyValue?: string;
}

interface Props {
  status: NodeStatus;
  config: BrambleConfig;
}

function hasBattery(status: NodeStatus): boolean {
  // Boards without a battery commonly report 0mV / 0%.
  if (status.batteryMv === undefined && status.batteryPct === undefined) return false;
  // `present` is hardware-verified; prefer it when the firmware reports it.
  // Older firmware omits it, so fall back to the voltage guess.
  if (status.present !== undefined) return status.present;
  return (status.batteryMv ?? 0) > 1000;
}

export function SystemInfo({ status, config }: Props) {
  const { identity } = config;
  const [copiedLabel, setCopiedLabel] = useState<string | null>(null);
  const copiedTimer = useRef<number | null>(null);

  useEffect(() => () => {
    if (copiedTimer.current !== null) {
      window.clearTimeout(copiedTimer.current);
    }
  }, []);

  const onCopy = async (label: string, text: string) => {
    const ok = await copyWithFallback(text);
    if (!ok) return;

    setCopiedLabel(label);
    if (copiedTimer.current !== null) {
      window.clearTimeout(copiedTimer.current);
    }
    copiedTimer.current = window.setTimeout(() => {
      setCopiedLabel(null);
    }, 1200);
  };

  // Heap health indicator
  const heapWarning = status.freeHeapBytes < 20_000;
  const heapDanger = status.freeHeapBytes < 8_000;

  const address = formatAddr0x(identity.address);
  const pubkeyHash = formatAddr0x(identity.pubkeyHash);

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
    ...((status.batteryPct !== undefined || status.batteryMv !== undefined)
      ? [{
          label: 'Battery',
          // While charging, battery_pct is derived from the charge rail
          // rather than the cell, so it is not a real level: hide it. The
          // mV reading is still a real measurement, just of the rail rather
          // than the cell, so it stays visible labeled as such.
          value: !hasBattery(status)
            ? 'N/A'
            : status.charging === 'yes'
              ? `⚡ Charging (${status.batteryMv ?? '?'} mV rail)`
              : `${status.batteryPct ?? '?'}% (${status.batteryMv ?? '?'} mV)`,
          color: !hasBattery(status)
            ? ('muted' as const)
            : status.charging === 'yes'
              ? undefined
              : (status.batteryPct ?? 100) < 10
                ? ('danger' as const)
                : (status.batteryPct ?? 100) < 25
                  ? ('warning' as const)
                  : undefined,
        }]
      : []),
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
      value: address,
      mono: true,
      copyValue: address,
    },
    {
      label: 'Pubkey Hash',
      value: pubkeyHash,
      mono: true,
      copyValue: pubkeyHash,
    },
    {
      label: 'Web App',
      value: __APP_VERSION__,
      mono: true,
      color: 'muted',
    },
  ];

  return (
    <section className={styles.card}>
      <h2 className={styles.heading}>ℹ️ System Info</h2>
      <dl className={styles.list}>
        {rows.map(({ label, value, mono, color, copyValue }) => {
          const isCopied = copiedLabel === label;
          return (
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
                <span className={styles.valueWrap}>
                  <span>{value}</span>
                  {copyValue && (
                    <button
                      type="button"
                      className={styles.copyBtn}
                      onClick={() => onCopy(label, copyValue)}
                      aria-label={`Copy ${label}`}
                      title={isCopied ? 'Copied!' : `Copy ${label}`}
                    >
                      <span aria-hidden="true">{isCopied ? '✓' : '📋'}</span>
                    </button>
                  )}
                </span>
              </dd>
            </div>
          );
        })}
      </dl>
    </section>
  );
}
