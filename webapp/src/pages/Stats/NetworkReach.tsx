import { useState, useEffect, useCallback } from 'react';
import { useStore } from '../../store/index';
import { sendProbe } from '../../store/actions';
import { IconProbe } from '../../components/Icons';
import type { ProbeResponse } from '../../types/bramble';
import styles from './NetworkReach.module.css';

function shortAddr(addr: number): string {
  return '0x' + (addr >>> 0).toString(16).toUpperCase().slice(-4);
}

function formatPath(r: ProbeResponse): string {
  if (r.hopCount <= 1 || r.relayPath.length === 0) return 'direct';
  return r.relayPath.map(a => '→ ' + shortAddr(a)).join(' ');
}

function hopClass(hops: number): string {
  if (hops <= 1) return styles.hop1;
  if (hops === 2) return styles.hop2;
  if (hops === 3) return styles.hop3;
  return styles.hop4;
}

function ResultsTable({ responses }: { responses: ProbeResponse[] }) {
  const sorted = [...responses].sort((a, b) => a.hopCount - b.hopCount);
  if (sorted.length === 0) return null;
  return (
    <table className={styles.table}>
      <thead>
        <tr>
          <th>Node</th>
          <th>Hops</th>
          <th>RSSI</th>
          <th>SNR</th>
          <th>Path</th>
        </tr>
      </thead>
      <tbody>
        {sorted.map(r => (
          <tr key={r.responderAddr}>
            <td>{shortAddr(r.responderAddr)}</td>
            <td>
              <span className={`${styles.hopBadge} ${hopClass(r.hopCount)}`}>
                {r.hopCount}
              </span>
            </td>
            <td>{r.rssi} dBm</td>
            <td>{r.snr.toFixed(1)}</td>
            <td className={styles.pathCell}>{formatPath(r)}</td>
          </tr>
        ))}
      </tbody>
    </table>
  );
}

export function NetworkReach() {
  const probeResult = useStore(s => s.probeResult);
  const collecting = useStore(s => s.probeCollecting);
  const isConnected = useStore(s => s.connectionState === 'connected');

  const [remaining, setRemaining] = useState(0);
  const [sending, setSending] = useState(false);

  // Countdown timer during collection
  useEffect(() => {
    if (!collecting || !probeResult) return;
    const update = () => {
      const elapsed = (Date.now() - probeResult.sentAt) / 1000;
      const left = Math.max(0, probeResult.ackWindow - elapsed);
      setRemaining(Math.ceil(left));
    };
    update();
    const id = setInterval(update, 1000);
    return () => clearInterval(id);
  }, [collecting, probeResult]);

  const handleSend = useCallback(async () => {
    setSending(true);
    try {
      await sendProbe();
    } finally {
      setSending(false);
    }
  }, []);

  const probeIdShort = probeResult
    ? (probeResult.probeId >>> 0).toString(16).slice(-4)
    : '';

  const sentTime = probeResult
    ? new Date(probeResult.sentAt).toLocaleTimeString([], { hour: 'numeric', minute: '2-digit' })
    : '';

  const progress = probeResult && collecting
    ? Math.min(100, ((Date.now() - probeResult.sentAt) / (probeResult.ackWindow * 1000)) * 100)
    : 0;

  return (
    <div className={styles.card}>
      <div className={styles.header}>
        <IconProbe size={18} className={styles.headerIcon} />
        <span className={styles.title}>Network Reach</span>
        {collecting && (
          <span className={styles.subtitle}>— Collecting… ({remaining}s remaining)</span>
        )}
        {probeResult && !collecting && (
          <span className={styles.subtitle}>— Probe #{probeIdShort}</span>
        )}
      </div>

      {!probeResult && !collecting && (
        <>
          <p className={styles.description}>Test how far your broadcasts reach.</p>
          <button
            className={styles.probeBtn}
            onClick={() => void handleSend()}
            disabled={!isConnected || sending}
          >
            Send Probe
          </button>
        </>
      )}

      {collecting && probeResult && (
        <>
          <div className={styles.progressWrap}>
            <div className={styles.progressBar}>
              <div className={styles.progressFill} style={{ width: `${progress}%` }} />
            </div>
          </div>
          <div className={styles.responseCount}>
            Responses: {probeResult.responses.length} so far
          </div>
          <ResultsTable responses={probeResult.responses} />
        </>
      )}

      {probeResult && !collecting && (
        <>
          <div className={styles.meta}>
            Sent: {sentTime} · Responses: {probeResult.responses.length} nodes
          </div>
          <ResultsTable responses={probeResult.responses} />
          <button
            className={styles.probeBtn}
            onClick={() => void handleSend()}
            disabled={!isConnected || sending}
            style={{ marginTop: '0.75rem' }}
          >
            Send Probe
          </button>
        </>
      )}
    </div>
  );
}
