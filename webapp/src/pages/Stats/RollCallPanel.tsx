import { useCallback, useEffect, useRef, useState } from 'react';
import { useStore } from '../../store/index';
import { loadRollCall, startRollCall } from '../../store/actions';
import { AddressLabel } from '../../components/AddressLabel';
import { formatAddrShort } from '../../utils/address';
import type { RollCallLedger } from '../../types/bramble';
import styles from './RollCallPanel.module.css';

/* While a roll-call is collecting the ledger changes on every answer, so poll
 * it; once it closes the ledger is final and polling stops. */
const OPEN_POLL_MS = 3_000;

function formatDuration(ms: number): string {
  const total = Math.max(0, Math.round(ms / 1000));
  const mins = Math.floor(total / 60);
  const secs = total % 60;
  return mins > 0 ? `${mins}m ${secs}s` : `${secs}s`;
}

function formatAt(ms: number | undefined): string {
  if (ms === undefined) return '-';
  return `${(ms / 1000).toFixed(1)}s in`;
}

function Ledger({ ledger }: { ledger: RollCallLedger }) {
  const peerNames = useStore(s => s.peerNames);
  const answered = ledger.responders.filter(r => r.responded);

  return (
    <>
      <div className={styles.summary}>
        <span className={styles.count} data-testid="rollcall-count">
          {ledger.anchored
            ? `${ledger.responded} of ${ledger.expected} expected`
            : `${ledger.responded} responded`}
        </span>
        <span className={styles.mode}>
          {ledger.anchored ? 'Anchored fleet' : 'Observed only (un-anchored)'}
        </span>
      </div>

      <p className={styles.modeNote}>
        {ledger.anchored
          ? 'The expected set is this node’s anchor-certified peers, so a member with no verified answer is named below.'
          : 'This node pins trust-on-first-use identities, so there is no authoritative expected set: the ledger lists the members that answered and cannot name anyone missing.'}
      </p>

      {answered.length > 0 && (
        <table className={styles.table}>
          <thead>
            <tr>
              <th>Member</th>
              <th title="Milliseconds into the roll-call at which the signed answer verified.">
                Answered
              </th>
              <th title="Announce round the answer named.">Round</th>
              <th title="Relay path, when the broadcast delivery receipt supplied one.">Path</th>
            </tr>
          </thead>
          <tbody>
            {answered.map(r => (
              <tr key={r.addr}>
                <td>
                  <div className={styles.memberCell}>
                    <AddressLabel addr={r.addr} name={peerNames.get(r.addr)} short={!peerNames.get(r.addr)} />
                    {peerNames.get(r.addr) && (
                      <span className={styles.memberSub}>{formatAddrShort(r.addr)}</span>
                    )}
                  </div>
                </td>
                <td>{formatAt(r.atMs)}</td>
                <td>{r.round ?? '-'}</td>
                <td className={styles.pathCell}>
                  {r.relayPath?.length
                    ? r.relayPath.map(a => formatAddrShort(a)).join(' → ')
                    : 'not reported'}
                </td>
              </tr>
            ))}
          </tbody>
        </table>
      )}

      {ledger.anchored && ledger.missing.length > 0 && (
        <div className={styles.missing} data-testid="rollcall-missing">
          <span className={styles.missingLabel}>No answer ({ledger.missingCount}):</span>
          {ledger.missing.map(addr => (
            <AddressLabel key={addr} addr={addr} name={peerNames.get(addr)} short={!peerNames.get(addr)} />
          ))}
        </div>
      )}

      <p className={styles.caveat}>
        A verified signature proves that member heard this roll-call and chose to
        answer. Silence proves nothing on its own: the member may be off, out of
        range, out of airtime budget, or suppressed by a relay that holds the
        network key.
      </p>

      {(ledger.unattested > 0 || ledger.overflow > 0 || ledger.late > 0 || ledger.pendingDropped > 0) && (
        <div className={styles.counters}>
          {ledger.unattested > 0 && <span>{ledger.unattested} unattested</span>}
          {ledger.overflow > 0 && <span>{ledger.overflow} over ledger capacity</span>}
          {ledger.late > 0 && <span>{ledger.late} after close</span>}
          {ledger.pendingDropped > 0 && <span>{ledger.pendingDropped} answers this node could not queue</span>}
        </div>
      )}
    </>
  );
}

export function RollCallPanel() {
  const isConnected = useStore(s => s.connectionState === 'connected');
  const [ledger, setLedger] = useState<RollCallLedger | null>(null);
  const [text, setText] = useState('');
  const [starting, setStarting] = useState(false);
  const [notice, setNotice] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);
  const mounted = useRef(true);

  useEffect(() => {
    mounted.current = true;
    return () => {
      mounted.current = false;
    };
  }, []);

  const refresh = useCallback(async () => {
    if (!isConnected) return;
    try {
      const next = await loadRollCall();
      if (mounted.current) {
        setLedger(next);
        setError(null);
      }
    } catch (e) {
      if (mounted.current) setError(e instanceof Error ? e.message : String(e));
    }
  }, [isConnected]);

  useEffect(() => {
    if (!isConnected) return;
    void refresh();
  }, [isConnected, refresh]);

  useEffect(() => {
    if (!isConnected || !ledger?.active || !ledger.open) return;
    const id = setInterval(() => void refresh(), OPEN_POLL_MS);
    return () => clearInterval(id);
  }, [isConnected, ledger?.active, ledger?.open, refresh]);

  const maxBytes = ledger?.maxTextBytes && ledger.maxTextBytes > 0 ? ledger.maxTextBytes : undefined;

  const handleStart = useCallback(async () => {
    setStarting(true);
    setNotice(null);
    setError(null);
    try {
      const res = await startRollCall(text);
      if (!res.ok) {
        const wait = res.retryAfterMs ? ` Try again in ${formatDuration(res.retryAfterMs)}.` : '';
        const why =
          res.reason === 'busy'
            ? 'A roll-call this node started is still collecting.'
            : res.reason === 'rate_limited'
              ? 'Roll-calls are rate limited: this one was too soon after the last.'
              : 'The announce was not transmitted, so nothing is owed an answer.';
        setNotice(`${why}${wait}`);
      } else {
        setText('');
      }
      await refresh();
    } catch (e) {
      setError(e instanceof Error ? e.message : String(e));
    } finally {
      if (mounted.current) setStarting(false);
    }
  }, [text, refresh]);

  const collecting = ledger?.active === true && ledger.open;
  const remainingMs = ledger ? Math.max(0, ledger.windowMs - ledger.elapsedMs) : 0;

  return (
    <div className={styles.card} data-testid="rollcall-panel">
      <div className={styles.header}>
        <span className={styles.title}>Roll Call</span>
        {ledger?.active && ledger.text && (
          <span className={styles.payload}>“{ledger.text}”</span>
        )}
        {collecting && (
          <span className={styles.subtitle}>
            collecting, {formatDuration(remainingMs)} left (round {ledger.roundsSent} of{' '}
            {ledger.roundsTotal})
          </span>
        )}
      </div>

      <p className={styles.description}>
        Ask every member to answer with an identity-signed reply, and keep a
        ledger of who did.
      </p>

      <div className={styles.form}>
        <input
          className={styles.input}
          type="text"
          value={text}
          maxLength={maxBytes}
          placeholder="Message (optional)"
          aria-label="Roll-call message"
          onChange={e => setText(e.target.value)}
          disabled={!isConnected || starting || collecting}
        />
        <button
          className={styles.startBtn}
          onClick={() => void handleStart()}
          disabled={!isConnected || starting || collecting}
        >
          Start roll call
        </button>
      </div>

      {notice && <p className={styles.notice} role="status">{notice}</p>}
      {error && <p className={styles.error} role="alert">{error}</p>}

      {!isConnected && <p className={styles.empty}>Connect to a node to run a roll call.</p>}
      {isConnected && ledger && !ledger.active && (
        <p className={styles.empty}>No roll call has run on this node yet.</p>
      )}
      {isConnected && ledger?.active && <Ledger ledger={ledger} />}
    </div>
  );
}
