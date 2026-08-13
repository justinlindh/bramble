import { useState, useEffect, useCallback, useMemo } from 'react';
import { useStore } from '../../store/index';
import { sendProbe } from '../../store/actions';
import { IconProbe } from '../../components/Icons';
import { NamedAddress } from '../../components/NamedAddress';
import { formatAddrShort } from '../../utils/address';
import type { ProbeResponse, ProbeResult } from '../../types/bramble';
import styles from './NetworkReach.module.css';

const PROBE_RESULTS_SESSION_KEY = 'bramble:network-reach:probe-result';

type PersistedProbePayload = {
  probeResult: ProbeResult;
  persistedAt: number;
};

function loadPersistedProbeResult(): PersistedProbePayload | null {
  try {
    const raw = sessionStorage.getItem(PROBE_RESULTS_SESSION_KEY);
    if (!raw) return null;
    const parsed = JSON.parse(raw) as PersistedProbePayload;
    if (!parsed || typeof parsed !== 'object') return null;
    if (!parsed.probeResult || typeof parsed.persistedAt !== 'number') return null;
    return parsed;
  } catch {
    return null;
  }
}

function savePersistedProbeResult(probeResult: ProbeResult): void {
  try {
    const payload: PersistedProbePayload = {
      probeResult,
      persistedAt: Date.now(),
    };
    sessionStorage.setItem(PROBE_RESULTS_SESSION_KEY, JSON.stringify(payload));
  } catch {
    // noop
  }
}

function formatAgeMinutes(ageMs: number): string {
  const mins = Math.max(1, Math.floor(ageMs / 60_000));
  return `Results from ${mins} minute${mins === 1 ? '' : 's'} ago`;
}

function formatPath(r: ProbeResponse): string {
  if (r.hopCount <= 1 || !r.relayPath?.length) return 'direct';
  return r.relayPath.map((a: number) => '→ ' + formatAddrShort(a)).join(' ');
}

function hopClass(hops: number): string {
  if (hops <= 1) return styles.hop1;
  if (hops === 2) return styles.hop2;
  if (hops === 3) return styles.hop3;
  return styles.hop4;
}

function confidenceColor(confidence: number): string {
  if (confidence >= 0.8) return '#3fb950';
  if (confidence >= 0.5) return '#d29922';
  return '#d73a49';
}

type ProbeRow = {
  responderAddr: number;
  response?: ProbeResponse;
};

function ResultsTable({ rows }: { rows: ProbeRow[] }) {
  const sorted = [...rows].sort((a, b) => {
    const aSeen = a.response?.seenRounds ?? 0;
    const bSeen = b.response?.seenRounds ?? 0;
    if (bSeen !== aSeen) return bSeen - aSeen;
    const aHops = a.response?.hopCount ?? 99;
    const bHops = b.response?.hopCount ?? 99;
    return aHops - bHops;
  });
  const peerNames = useStore(s => s.peerNames);
  if (sorted.length === 0) return null;
  return (
    <table className={styles.table}>
      <thead>
        <tr>
          <th>Node</th>
          <th title="Number of relay nodes between you and this node. 1 = direct neighbor.">Hops</th>
          <th title="How confidently this node appears reachable based on probe responses.">Confidence</th>
          <th title="Received Signal Strength (dBm). Closer to 0 is stronger. Above −90 is good, below −110 is weak.">RSSI</th>
          <th title="Signal-to-Noise Ratio (dB). Higher is better. Above 0 means signal is stronger than noise.">SNR</th>
          <th title="The chain of relay nodes this probe passed through to reach the destination.">Path</th>
        </tr>
      </thead>
      <tbody>
        {sorted.map(({ responderAddr, response }) => {
          const name = peerNames.get(responderAddr);
          return (
          <tr key={responderAddr}>
            <td>
              <div className={styles.nodeCell}>
                <NamedAddress addr={responderAddr} name={name} subClassName={styles.nodeSub} />
              </div>
            </td>
            <td>
              {response ? (
                <span className={`${styles.hopBadge} ${hopClass(response.hopCount)}`}>
                  {response.hopCount}
                </span>
              ) : '-'}
            </td>
            <td style={response ? { color: confidenceColor(response.confidence ?? 1) } : undefined}>
              {response ? `${Math.round((response.confidence ?? 1) * 100)}%` : '-'}
            </td>
            <td>{response ? `${response.rssi} dBm` : 'no response'}</td>
            <td>{response ? response.snr.toFixed(1) : '-'}</td>
            <td className={styles.pathCell}>{response ? formatPath(response) : '-'}</td>
          </tr>
        )})}
      </tbody>
    </table>
  );
}

export function NetworkReach() {
  const probeResult = useStore(s => s.probeResult);
  /* A probe is collecting exactly while it exists and is not yet finalized.
   * Finalization happens on bramble.onProbeComplete or at the ack-window
   * fallback (both set complete: true), so this is the single source of
   * truth rather than a second boolean kept in sync by hand. */
  const collecting = probeResult != null && !probeResult.complete;
  const isConnected = useStore(s => s.connectionState === 'connected');
  const neighbors = useStore(s => s.neighbors);
  const selfAddr = useStore(s => s.config?.identity.address);
  const setProbeResult = useStore(s => s.setProbeResult);

  const rows = useMemo(() => {
    const responseMap = new Map<number, ProbeResponse>();
    for (const r of probeResult?.responses ?? []) responseMap.set(r.responderAddr, r);

    const known = new Set<number>();
    for (const n of neighbors ?? []) {
      if (selfAddr !== undefined && n.addr === selfAddr) continue;
      known.add(n.addr);
    }
    for (const addr of responseMap.keys()) known.add(addr);

    return [...known].map(addr => ({ responderAddr: addr, response: responseMap.get(addr) }));
  }, [probeResult, neighbors, selfAddr]);

  const [remaining, setRemaining] = useState(0);
  const [sending, setSending] = useState(false);
  const [persistedAt, setPersistedAt] = useState<number | null>(null);

  useEffect(() => {
    if (probeResult) return;
    const persisted = loadPersistedProbeResult();
    if (!persisted) return;
    /* A restored probe has no live collection window (its ack-window
     * fallback timer did not survive the reload), so it is always shown as
     * finalized rather than resuming a countdown that can never complete. */
    setProbeResult({ ...persisted.probeResult, complete: true });
    setPersistedAt(persisted.persistedAt);
  }, [probeResult, setProbeResult]);

  useEffect(() => {
    if (!probeResult) return;
    savePersistedProbeResult(probeResult);
    if (collecting) setPersistedAt(null);
  }, [probeResult, collecting]);

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
    setPersistedAt(null);
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

  const staleLabel = persistedAt ? formatAgeMinutes(Date.now() - persistedAt) : null;

  return (
    <div className={styles.card}>
      <div className={styles.header}>
        <IconProbe size={18} className={styles.headerIcon} />
        <span className={styles.title}>Network Reach</span>
        {collecting && (
          <span className={styles.subtitle}>: Collecting… ({remaining}s remaining)</span>
        )}
        {probeResult && !collecting && (
          <span className={styles.subtitle}>: Probe #{probeIdShort}</span>
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
          <ResultsTable rows={rows} />
        </>
      )}

      {probeResult && !collecting && (
        <>
          <div className={styles.meta}>
            Sent: {sentTime} · Responses: {probeResult.responses.length} nodes
            {staleLabel && ` · ${staleLabel}`}
          </div>
          <ResultsTable rows={rows} />
          <button
            className={styles.probeBtn}
            onClick={() => void handleSend()}
            disabled={!isConnected || sending}
            style={{ marginTop: '0.75rem' }}
          >
            Refresh
          </button>
        </>
      )}
    </div>
  );
}
