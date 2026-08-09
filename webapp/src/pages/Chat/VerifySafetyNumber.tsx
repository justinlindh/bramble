import { useState } from 'react';
import { QRShareModal } from '../../components/QRShareModal';
import { IconLock, IconWarning } from '../../components/Icons';
import styles from './VerifySafetyNumber.module.css';

/**
 * Groups a 7-digit SAS into "123 4567", matching the pager's
 * sas_format_grouped (components/ui_graphics/sas_format.c) so the code reads
 * identically on both surfaces. Returns the raw string unchanged if it is
 * not exactly 7 digits (e.g. the empty "no pin yet" string).
 */
function formatSasGrouped(sas: string): string {
  if (!/^\d{7}$/.test(sas)) return sas;
  return `${sas.slice(0, 3)} ${sas.slice(3)}`;
}

interface VerifySafetyNumberProps {
  /** Peer address as hex, no 0x prefix (matches the store/RPC convention). */
  peerAddress: string;
  /** 7-digit SAS, or "" if this peer has no pin yet (never DM'd). */
  sas: string;
  verified: boolean;
  /** RAM-only: the peer's identity key changed since the last verify. */
  keyChanged: boolean;
  onSetVerified: (peerAddress: string, verified: boolean) => unknown;
  /** Optional display name; falls back to the hex address. */
  peerName?: string;
  /** When set, renders a "back to chat" affordance above the title. */
  onClose?: () => void;
}

export function VerifySafetyNumber({
  peerAddress,
  sas,
  verified,
  keyChanged,
  onSetVerified,
  peerName,
  onClose,
}: VerifySafetyNumberProps) {
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [showQr, setShowQr] = useState(false);

  const hasSas = /^\d{7}$/.test(sas);

  const handleToggleVerified = async () => {
    setError(null);
    setBusy(true);
    try {
      await onSetVerified(peerAddress, !verified);
    } catch (err) {
      setError((err as Error)?.message || 'Failed to update verification');
    } finally {
      setBusy(false);
    }
  };

  return (
    <div className={styles.panel}>
      {onClose && (
        <button className={styles.backBtn} onClick={onClose}>← Back to chat</button>
      )}
      <h3 className={styles.title}>Verify safety number</h3>
      <p className={styles.subtitle}>
        with {peerName ?? `0x${peerAddress.toUpperCase()}`}
      </p>

      {keyChanged && (
        <div className={styles.keyChangedBanner} role="alert">
          <span className={styles.keyChangedIcon}><IconWarning size={16} /></span>
          <div>
            <p className={styles.keyChangedTitle}>Warning</p>
            <p className={styles.keyChangedText}>
              The safety number changed. This can happen if your contact reinstalled, or
              it can mean someone is intercepting. Re-verify before trusting.
            </p>
          </div>
        </div>
      )}

      {hasSas ? (
        <>
          <p className={styles.hint}>
            Read this code aloud with your contact through a trusted channel (in person, a
            phone call). It must match on both devices.
          </p>
          <div className={styles.sasCode} aria-label="Safety number code">
            {formatSasGrouped(sas)}
          </div>

          <div className={styles.status}>
            {verified ? (
              <span className={styles.statusVerified}>
                <IconLock size={14} /> Verified
              </span>
            ) : (
              <span className={styles.statusUnverified}>Not verified</span>
            )}
          </div>

          <div className={styles.actions}>
            <button
              className={styles.verifyBtn}
              onClick={handleToggleVerified}
              disabled={busy}
            >
              {busy ? 'Working…' : verified ? 'Unverify' : 'Mark verified'}
            </button>
            <button className={styles.qrBtn} onClick={() => setShowQr(true)}>
              Show QR
            </button>
          </div>
          {error && <p className={styles.error}>{error}</p>}
        </>
      ) : (
        <p className={styles.hint}>
          No safety number yet. Send this contact a message first, then come back here to
          verify.
        </p>
      )}

      {showQr && (
        <QRShareModal
          title="Safety number"
          shareString={`bramble://sas/v1?addr=${peerAddress}&sas=${sas}`}
          description="Scan on the other device to compare safety numbers side by side."
          onClose={() => setShowQr(false)}
        />
      )}
    </div>
  );
}
