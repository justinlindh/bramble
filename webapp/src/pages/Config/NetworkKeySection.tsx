import { useEffect, useState } from 'react';
import { useCopyFlash } from '../../hooks/useCopyFlash';
import { encodeNetworkKeyShare, parseNetworkKeyShare } from '../../utils/networkKeyShare';
import { setNetworkKey, generateNetworkKey, loadNetworkKeyStatus } from '../../store/actions';
import { useStore } from '../../store/index';
import { QRShareModal } from '../../components/QRShareModal';
import { QRScanModal } from '../../components/QRScanModal';
import type { ScanResult } from '../../components/QRScanModal';
import { friendlyErrorFrom } from '../../lib/errors';
import styles from './NetworkKeySection.module.css';

const HEX64 = /^[0-9a-fA-F]{64}$/;

// Provisioning the control-plane network key. Two honest paths:
//   - FOUND a network: this node mints a fresh key on-device (entropy-gated,
//     provisioned + persisted immediately) and shows it once so the operator
//     can copy it to every other node. This node becomes the fleet founder.
//   - JOIN a network: paste or scan a key from a node that already has one.
// The key is a write-only secret: the device never reads a stored key back,
// only the one-way fingerprint used to confirm nodes are on the same key.
export function NetworkKeySection() {
  // The provisioning status lives in the global store (polled by the app shell
  // to drive the top-level UNPROVISIONED banner); read it here so this section
  // and the banner never disagree.
  const status = useStore((s) => s.networkKeyStatus);
  const [statusError, setStatusError] = useState<string | null>(null);

  const [generated, setGenerated] = useState<{ hex: string; uri: string; fp: string } | null>(null);
  const [generating, setGenerating] = useState(false);
  const [confirmRekey, setConfirmRekey] = useState(false);
  const [generateError, setGenerateError] = useState<string | null>(null);
  const [showGeneratedShare, setShowGeneratedShare] = useState(false);
  const [hexCopied, copyHex, resetHexCopied] = useCopyFlash(2000);

  const [pasteInput, setPasteInput] = useState('');
  const [showScan, setShowScan] = useState(false);
  const [provisioning, setProvisioning] = useState(false);
  const [provisionError, setProvisionError] = useState<string | null>(null);
  const [provisionSuccess, setProvisionSuccess] = useState<string | null>(null);

  const refreshStatus = async () => {
    try {
      await loadNetworkKeyStatus();
      setStatusError(null);
    } catch (e) {
      setStatusError(friendlyErrorFrom(e));
    }
  };

  useEffect(() => {
    void refreshStatus();
  }, []);

  // ── Found (generate on-device) ─────────────────────────────────────────────
  const onGenerate = async () => {
    // Re-keying an already-provisioned node is destructive: warn first.
    if (status?.provisioned && !confirmRekey) {
      setConfirmRekey(true);
      return;
    }
    setConfirmRekey(false);
    setGenerating(true);
    setGenerateError(null);
    try {
      const { key, fingerprint } = await generateNetworkKey();
      setGenerated({ hex: key, uri: encodeNetworkKeyShare(key), fp: fingerprint });
      resetHexCopied();
      await refreshStatus();
    } catch (e) {
      setGenerateError(friendlyErrorFrom(e));
    } finally {
      setGenerating(false);
    }
  };

  const handleCopyHex = () => {
    if (generated) void copyHex(generated.hex);
  };

  // ── Join (paste/scan an existing key) ──────────────────────────────────────
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
        setProvisionSuccess('Network key provisioned. This node joined the network.');
        await refreshStatus();
      } else {
        setProvisionError('Device rejected the key.');
      }
    } catch (e) {
      setProvisionError(friendlyErrorFrom(e));
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
        {status &&
          (status.provisioned ? (
            <p className={styles.notice}>
              Provisioned. Fingerprint{' '}
              <span className={styles.fingerprint}>{status.fingerprint}</span>. Every node in this
              network should show this same fingerprint.
            </p>
          ) : (
            <p className={styles.warning}>
              UNPROVISIONED: this node has no network key, so it is INERT (not meshing). Found a
              network below, or join one, to bring it online.
            </p>
          ))}
      </div>

      {/* ── Found a network (generate on-device) ── */}
      <div className={styles.subsection}>
        <h3 className={styles.subheading}>Found a new network</h3>
        <p className={styles.hint}>
          Mints a fresh key on this node and provisions it here immediately, making this node the
          founder. Copy the key it shows to every other node (below) so they can join. The key is
          shown once and never read back.
        </p>
        {confirmRekey ? (
          <div className={styles.warnBox}>
            <p className={styles.warning}>
              This node is already provisioned (fingerprint{' '}
              <span className={styles.fingerprint}>{status?.fingerprint}</span>). Generating a new
              key RE-KEYS this node and cuts it off from any node still on the old key until you copy
              the new key to them. Continue?
            </p>
            <div className={styles.row}>
              <button className={styles.dangerBtn} type="button" onClick={onGenerate} disabled={generating}>
                {generating ? 'Re-keying...' : 'Re-key this node'}
              </button>
              <button className={styles.ghostBtn} type="button" onClick={() => setConfirmRekey(false)}>
                Cancel
              </button>
            </div>
          </div>
        ) : (
          <div className={styles.row}>
            <button className={styles.primaryBtn} onClick={onGenerate} disabled={generating}>
              {generating ? 'Generating...' : status?.provisioned ? 'Generate new key (re-key)' : 'Generate key'}
            </button>
          </div>
        )}
        {generateError && <p className={styles.error}>{generateError}</p>}
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
              Fingerprint <span className={styles.fingerprint}>{generated.fp}</span>. Copy this key
              to your other nodes and confirm each one reports this fingerprint.
            </p>
            <div className={styles.row}>
              <button className={styles.primaryBtn} type="button" onClick={() => setShowGeneratedShare(true)}>
                Show QR
              </button>
            </div>
          </>
        )}
      </div>

      {/* ── Join a network (paste/scan) ── */}
      <div className={styles.subsection}>
        <h3 className={styles.subheading}>Join an existing network</h3>
        <p className={styles.hint}>Scan or paste the key from a node that already has one.</p>
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
