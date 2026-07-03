import { useEffect, useState } from 'react';
import {
  encodeNetworkKeyShare,
  parseNetworkKeyShare,
  networkKeyFingerprint,
  generateNetworkKeyHex,
} from '../../utils/networkKeyShare';
import { setNetworkKey, getNetworkKeyStatus } from '../../store/actions';
import { QRShareModal } from '../../components/QRShareModal';
import { QRScanModal } from '../../components/QRScanModal';
import type { ScanResult } from '../../components/QRScanModal';
import styles from './NetworkKeySection.module.css';

const HEX64 = /^[0-9a-fA-F]{64}$/;

// The network key is a write-only secret: this section can generate one and
// carry it out-of-band (QR / copy-paste) to each node, or provision this node
// from a share made elsewhere. It never reads a key back from the device,
// only the one-way fingerprint used to confirm nodes are on the same key.
export function NetworkKeySection() {
  const [status, setStatus] = useState<{ provisioned: boolean; fingerprint: string } | null>(null);
  const [statusError, setStatusError] = useState<string | null>(null);

  const [generated, setGenerated] = useState<{ hex: string; uri: string; fp: string } | null>(null);
  const [generating, setGenerating] = useState(false);
  const [showGeneratedShare, setShowGeneratedShare] = useState(false);
  const [hexCopied, setHexCopied] = useState(false);

  const [pasteInput, setPasteInput] = useState('');
  const [showScan, setShowScan] = useState(false);
  const [provisioning, setProvisioning] = useState(false);
  const [provisionError, setProvisionError] = useState<string | null>(null);
  const [provisionSuccess, setProvisionSuccess] = useState<string | null>(null);

  const refreshStatus = async () => {
    try {
      setStatus(await getNetworkKeyStatus());
      setStatusError(null);
    } catch (e) {
      setStatusError((e as Error).message);
    }
  };

  useEffect(() => {
    void refreshStatus();
  }, []);

  // ── Generate ──────────────────────────────────────────────────────────────
  const onGenerate = async () => {
    setGenerating(true);
    try {
      const hex = generateNetworkKeyHex();
      const fp = await networkKeyFingerprint(hex);
      setGenerated({ hex, uri: encodeNetworkKeyShare(hex), fp });
      setHexCopied(false);
    } finally {
      setGenerating(false);
    }
  };

  const handleCopyHex = async () => {
    if (!generated) return;
    try {
      await navigator.clipboard.writeText(generated.hex);
      setHexCopied(true);
      setTimeout(() => setHexCopied(false), 2000);
    } catch {
      // Clipboard API unavailable; the field below is still selectable/copyable by hand.
    }
  };

  // ── Provision ─────────────────────────────────────────────────────────────
  const onProvision = async (input: string) => {
    const parsed = parseNetworkKeyShare(input);
    const keyHex = parsed.ok
      ? parsed.data.key
      : HEX64.test(input.trim())
        ? input.trim().toLowerCase()
        : null;
    if (!keyHex) {
      setProvisionError('Enter a bramble://net/v1?k=... string or 64 hex chars.');
      return;
    }
    setProvisioning(true);
    setProvisionError(null);
    setProvisionSuccess(null);
    try {
      const ok = await setNetworkKey(keyHex);
      if (ok) {
        setPasteInput('');
        setProvisionSuccess('Network key provisioned.');
        await refreshStatus();
      } else {
        setProvisionError('Device rejected the key.');
      }
    } catch (e) {
      setProvisionError((e as Error).message);
    } finally {
      setProvisioning(false);
    }
  };

  const handlePasteSubmit = (e: React.FormEvent) => {
    e.preventDefault();
    void onProvision(pasteInput);
  };

  const handleScanResult = (result: ScanResult) => {
    setShowScan(false);
    if (result.kind !== 'network') {
      setProvisionError('Scanned a channel or node share, not a network key.');
      return;
    }
    void onProvision(result.data.key);
  };

  return (
    <div className={styles.section}>
      {/* ── Status ── */}
      <div className={styles.subsection}>
        <h3 className={styles.subheading}>Status</h3>
        {statusError && <p className={styles.error}>{statusError}</p>}
        {!statusError && !status && <p className={styles.muted}>Loading status...</p>}
        {status && (
          status.provisioned ? (
            <p className={styles.notice}>
              Provisioned (fingerprint <span className={styles.fingerprint}>{status.fingerprint}</span>)
            </p>
          ) : (
            <p className={styles.warning}>
              UNPROVISIONED: control plane is forgeable on the public fallback key
              (fingerprint <span className={styles.fingerprint}>{status.fingerprint}</span>). Generate and
              provision a network key on every node to close this off.
            </p>
          )
        )}
      </div>

      {/* ── Generate ── */}
      <div className={styles.subsection}>
        <h3 className={styles.subheading}>Generate a new key</h3>
        <p className={styles.hint}>
          Creates a random 32-byte key in this browser. It is never sent anywhere until you
          provision it below, and it is never shown again after you navigate away.
        </p>
        <div className={styles.row}>
          <button className={styles.primaryBtn} onClick={onGenerate} disabled={generating}>
            {generating ? 'Generating...' : 'Generate key'}
          </button>
        </div>
        {generated && (
          <>
            <div className={styles.row}>
              <span className={styles.label}>Key (hex)</span>
              <input
                className={styles.input}
                type="text"
                readOnly
                value={generated.hex}
                onFocus={(e) => e.target.select()}
                aria-label="Generated network key hex"
              />
              <button className={styles.ghostBtn} type="button" onClick={handleCopyHex}>
                {hexCopied ? 'Copied!' : 'Copy'}
              </button>
            </div>
            <p className={styles.hint}>
              Fingerprint <span className={styles.fingerprint}>{generated.fp}</span>. Verify this matches
              every node's status after you provision them.
            </p>
            <div className={styles.row}>
              <button className={styles.primaryBtn} type="button" onClick={() => setShowGeneratedShare(true)}>
                Show QR
              </button>
            </div>
          </>
        )}
      </div>

      {/* ── Provision ── */}
      <div className={styles.subsection}>
        <h3 className={styles.subheading}>Provision this node</h3>
        <p className={styles.hint}>Scan or paste a network-key share to set this node's key.</p>
        <div className={styles.row}>
          <button className={styles.primaryBtn} type="button" onClick={() => { setProvisionError(null); setShowScan(true); }}>
            Scan QR
          </button>
        </div>
        <form className={styles.row} onSubmit={handlePasteSubmit}>
          <input
            className={styles.input}
            type="text"
            value={pasteInput}
            onChange={(e) => setPasteInput(e.target.value)}
            placeholder="bramble://net/v1?k=... or 64 hex chars"
            aria-label="Network key share string"
            autoComplete="off"
          />
          <button className={styles.primaryBtn} type="submit" disabled={provisioning || !pasteInput.trim()}>
            {provisioning ? 'Provisioning...' : 'Provision'}
          </button>
        </form>
        {provisionError && <p className={styles.error}>{provisionError}</p>}
        {provisionSuccess && <p className={styles.notice}>{provisionSuccess}</p>}
      </div>

      {/* ── Generated key QR share modal ── */}
      {showGeneratedShare && generated && (
        <QRShareModal
          title="New network key"
          shareString={generated.uri}
          description={`Fingerprint ${generated.fp}. Scan this on each node to provision the same key.`}
          onClose={() => setShowGeneratedShare(false)}
        />
      )}

      {/* ── QR scan modal ── */}
      {showScan && (
        <QRScanModal
          title="Provision network key"
          onResult={handleScanResult}
          onClose={() => setShowScan(false)}
        />
      )}
    </div>
  );
}
