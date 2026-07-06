import { useEffect, useState } from 'react';
import {
  generateAnchorKeypair,
  anchorPubFromSeed,
  anchorFingerprint,
  signEndorsement,
  PERMANENT_NOT_AFTER,
} from '../../utils/anchor';
import {
  encodeAnchorBackup,
  parseAnchorBackup,
  encodeIdentityShare,
  parseIdentityShare,
  encodeCertShare,
  parseCertShare,
} from '../../utils/anchorShare';
import { setAnchor, getIdentity, setEndorsement, loadAnchorStatus } from '../../store/actions';
import { useStore } from '../../store/index';
import { QRShareModal } from '../../components/QRShareModal';
import styles from './AnchorSection.module.css';

const HEX64 = /^[0-9a-fA-F]{64}$/;

// Where the operator's SECRET anchor seed lives: localStorage in THIS browser
// only, mirroring how the app persists other client-side state (bramble:*).
// The seed is the fleet's root of trust; it is never sent to a node or over any
// RPC. setAnchor pushes the PUBLIC key; enrollment signs locally and sends only
// the resulting cert (not_after + signature).
const ANCHOR_SEED_KEY = 'bramble.anchor.seed';

interface ClientAnchor {
  seedHex: string;
  pubHex: string;
  fp: string;
}

function clientAnchorFromSeed(seedHex: string): ClientAnchor {
  const pubHex = anchorPubFromSeed(seedHex);
  return { seedHex, pubHex, fp: anchorFingerprint(pubHex) };
}

function loadStoredSeed(): string | null {
  try {
    const raw = localStorage.getItem(ANCHOR_SEED_KEY);
    return raw && HEX64.test(raw.trim()) ? raw.trim().toLowerCase() : null;
  } catch {
    return null;
  }
}

function storeSeed(seedHex: string): void {
  try {
    localStorage.setItem(ANCHOR_SEED_KEY, seedHex);
  } catch {
    // localStorage unavailable (private browsing); the operator still has the
    // backup string they were forced to save, so the anchor is not lost.
  }
}

function clearStoredSeed(): void {
  try {
    localStorage.removeItem(ANCHOR_SEED_KEY);
  } catch {
    // noop
  }
}

// The trust anchor is the fleet's Sybil-scarcity root. The operator's client
// holds the anchor PRIVATE seed offline (localStorage, this browser only),
// provisions the anchor PUBLIC key to each node, and enrolls a node by signing
// an endorsement cert over that node's identity key. The private seed NEVER
// leaves this client: provisioning sends the public key, enrollment sends the
// signed cert. Losing the seed with no backup means no new nodes can ever be
// enrolled (a re-anchor flag day); leaking it lets a Sybil enroll until the
// fleet re-anchors. That is why backup at creation is mandatory and gated.
export function AnchorSection() {
  // Node-side anchor status lives in the global store (same slice the enrollment
  // flow refreshes) so this section and any future banner never disagree.
  const nodeStatus = useStore((s) => s.anchorStatus);

  const [clientAnchor, setClientAnchor] = useState<ClientAnchor | null>(() => {
    const seed = loadStoredSeed();
    return seed ? clientAnchorFromSeed(seed) : null;
  });

  // Mandatory-backup gate: a freshly generated anchor is held ONLY here (never
  // persisted, never on a node) until the operator confirms they saved the
  // backup. Cancelling discards it. This is the load-bearing custody step.
  const [pendingAnchor, setPendingAnchor] = useState<ClientAnchor | null>(null);
  const [generateError, setGenerateError] = useState<string | null>(null);
  const [showPendingQR, setShowPendingQR] = useState(false);
  const [backupCopied, setBackupCopied] = useState(false);

  const [importInput, setImportInput] = useState('');
  const [importError, setImportError] = useState<string | null>(null);
  const [showExport, setShowExport] = useState(false);
  const [confirmForget, setConfirmForget] = useState(false);

  const [statusError, setStatusError] = useState<string | null>(null);
  const [provisioning, setProvisioning] = useState(false);
  const [provisionError, setProvisionError] = useState<string | null>(null);
  const [provisionSuccess, setProvisionSuccess] = useState<string | null>(null);

  const [enrolling, setEnrolling] = useState(false);
  const [enrollError, setEnrollError] = useState<string | null>(null);
  const [enrollSuccess, setEnrollSuccess] = useState<string | null>(null);

  const [remoteInput, setRemoteInput] = useState('');
  const [remoteError, setRemoteError] = useState<string | null>(null);
  const [remoteCert, setRemoteCert] = useState<{ uri: string; fp: string } | null>(null);
  const [showRemoteQR, setShowRemoteQR] = useState(false);

  const [identityShare, setIdentityShare] = useState<string | null>(null);
  const [identityError, setIdentityError] = useState<string | null>(null);
  const [showIdentityQR, setShowIdentityQR] = useState(false);

  const [applyInput, setApplyInput] = useState('');
  const [applyError, setApplyError] = useState<string | null>(null);
  const [applySuccess, setApplySuccess] = useState<string | null>(null);
  const [applying, setApplying] = useState(false);

  const refreshStatus = async () => {
    try {
      await loadAnchorStatus();
      setStatusError(null);
    } catch (e) {
      setStatusError((e as Error).message);
    }
  };

  useEffect(() => {
    void refreshStatus();
  }, []);

  // Whether the node is pinned to a DIFFERENT anchor than the client holds. A
  // cert signed by the client anchor would be rejected by such a node, so we
  // warn instead of minting a dead cert.
  const nodeFingerprint = nodeStatus?.anchored ? nodeStatus.anchor_fingerprint : undefined;
  const fingerprintMismatch = Boolean(
    clientAnchor && nodeFingerprint && nodeFingerprint !== clientAnchor.fp,
  );

  // -- Generate a new anchor (mandatory-backup flow) --------------------------
  const onGenerate = () => {
    setGenerateError(null);
    setBackupCopied(false);
    try {
      const { seedHex } = generateAnchorKeypair();
      // Held in component state only. NOT written to localStorage and NOT sent
      // anywhere until the operator confirms the backup below.
      setPendingAnchor(clientAnchorFromSeed(seedHex));
    } catch (e) {
      setGenerateError((e as Error).message);
    }
  };

  const onConfirmBackup = () => {
    if (!pendingAnchor) return;
    storeSeed(pendingAnchor.seedHex);
    setClientAnchor(pendingAnchor);
    setPendingAnchor(null);
    setShowPendingQR(false);
  };

  const onCancelPending = () => {
    // Discard the unsaved anchor entirely; it was never persisted.
    setPendingAnchor(null);
    setShowPendingQR(false);
  };

  const handleCopyBackup = async () => {
    if (!pendingAnchor) return;
    try {
      await navigator.clipboard.writeText(encodeAnchorBackup(pendingAnchor.seedHex));
      setBackupCopied(true);
      setTimeout(() => setBackupCopied(false), 2000);
    } catch {
      // Clipboard unavailable; the field is still selectable/copyable by hand.
    }
  };

  // -- Import / restore an anchor backup --------------------------------------
  const onImport = () => {
    setImportError(null);
    const seed = parseAnchorBackup(importInput) ?? (HEX64.test(importInput.trim()) ? importInput.trim().toLowerCase() : null);
    if (!seed) {
      setImportError('Enter a bramble://anchor/v1?sk=... backup or 64 hex chars.');
      return;
    }
    try {
      const anchor = clientAnchorFromSeed(seed);
      storeSeed(anchor.seedHex);
      setClientAnchor(anchor);
      setImportInput('');
    } catch (e) {
      setImportError((e as Error).message);
    }
  };

  const onForget = () => {
    clearStoredSeed();
    setClientAnchor(null);
    setConfirmForget(false);
    setProvisionSuccess(null);
    setEnrollSuccess(null);
    setRemoteCert(null);
  };

  // -- Provision the anchor PUBLIC key to the connected node ------------------
  const onProvision = async () => {
    if (!clientAnchor) return;
    setProvisioning(true);
    setProvisionError(null);
    setProvisionSuccess(null);
    try {
      const ok = await setAnchor(clientAnchor.pubHex);
      if (ok) {
        setProvisionSuccess('Anchor provisioned to this node.');
        await refreshStatus();
      } else {
        setProvisionError('Device rejected the anchor.');
      }
    } catch (e) {
      setProvisionError((e as Error).message);
    } finally {
      setProvisioning(false);
    }
  };

  // -- Enroll the connected node (local) --------------------------------------
  const onEnrollLocal = async () => {
    if (!clientAnchor) return;
    setEnrolling(true);
    setEnrollError(null);
    setEnrollSuccess(null);
    try {
      const identity = await getIdentity();
      // Sign locally with the anchor seed; only the resulting cert is sent.
      const { notAfterHex, sigHex } = signEndorsement(
        clientAnchor.seedHex,
        identity.ed25519_pub,
        PERMANENT_NOT_AFTER,
      );
      const ok = await setEndorsement(notAfterHex, sigHex);
      if (ok) {
        setEnrollSuccess('This node is enrolled (permanent endorsement applied).');
        await refreshStatus();
      } else {
        setEnrollError('Device rejected the endorsement cert.');
      }
    } catch (e) {
      setEnrollError((e as Error).message);
    } finally {
      setEnrolling(false);
    }
  };

  // -- Remote enrollment: sign a cert for a node's pasted identity share ------
  const onRemoteSign = () => {
    if (!clientAnchor) return;
    setRemoteError(null);
    setRemoteCert(null);
    const pub = parseIdentityShare(remoteInput) ?? (HEX64.test(remoteInput.trim()) ? remoteInput.trim().toLowerCase() : null);
    if (!pub) {
      setRemoteError('Enter a bramble://ident/v1?pk=... identity share or 64 hex chars.');
      return;
    }
    try {
      const { notAfterHex, sigHex } = signEndorsement(clientAnchor.seedHex, pub, PERMANENT_NOT_AFTER);
      setRemoteCert({ uri: encodeCertShare(notAfterHex, sigHex), fp: anchorFingerprint(pub) });
    } catch (e) {
      setRemoteError((e as Error).message);
    }
  };

  // -- Node-side: show this node's identity for a remote operator to enroll ---
  const onShowIdentity = async () => {
    setIdentityError(null);
    try {
      const identity = await getIdentity();
      setIdentityShare(encodeIdentityShare(identity.ed25519_pub));
    } catch (e) {
      setIdentityError((e as Error).message);
    }
  };

  // -- Node-side: apply a cert produced by a remote operator ------------------
  const onApply = async () => {
    setApplyError(null);
    setApplySuccess(null);
    const cert = parseCertShare(applyInput);
    if (!cert) {
      setApplyError('Enter a bramble://endorse/v1?na=...&sig=... cert share.');
      return;
    }
    setApplying(true);
    try {
      const ok = await setEndorsement(cert.notAfterHex, cert.sigHex);
      if (ok) {
        setApplyInput('');
        setApplySuccess('Endorsement applied. This node is enrolled.');
        await refreshStatus();
      } else {
        setApplyError('Device rejected the cert (wrong anchor, or not for this node).');
      }
    } catch (e) {
      setApplyError((e as Error).message);
    } finally {
      setApplying(false);
    }
  };

  return (
    <div className={styles.section}>
      {/* -- Anchor custody (this browser) -- */}
      <div className={styles.subsection}>
        <h3 className={styles.subheading}>Fleet anchor (held in this browser)</h3>
        {clientAnchor ? (
          <>
            <p className={styles.notice}>
              Anchor held. Fingerprint{' '}
              <span className={styles.fingerprint}>{clientAnchor.fp}</span>. The private seed is
              stored only in this browser and is never sent to a node.
            </p>
            <div className={styles.row}>
              <button className={styles.primaryBtn} type="button" onClick={() => setShowExport(true)}>
                Show backup again
              </button>
              {confirmForget ? (
                <>
                  <span className={styles.warning}>Forget the anchor from this browser?</span>
                  <button className={styles.dangerBtn} type="button" onClick={onForget}>
                    Forget anchor
                  </button>
                  <button className={styles.ghostBtn} type="button" onClick={() => setConfirmForget(false)}>
                    Cancel
                  </button>
                </>
              ) : (
                <button className={styles.ghostBtn} type="button" onClick={() => setConfirmForget(true)}>
                  Forget anchor
                </button>
              )}
            </div>
            <p className={styles.hint}>
              Forgetting only clears it here. If you have no backup, the fleet can never enroll a new
              node without a re-anchor flag day.
            </p>
          </>
        ) : pendingAnchor ? (
          <div className={styles.warnBox}>
            <p className={styles.warning}>
              BACK UP THIS ANCHOR NOW. This is your fleet's root secret, stored only in THIS browser.
              If you lose it, no new node can ever be enrolled without re-anchoring the whole fleet.
              If it leaks, a Sybil can enroll until you re-anchor. It is NOT saved until you confirm
              below.
            </p>
            <div className={styles.row}>
              <span className={styles.label}>Backup</span>
              <input
                className={styles.input}
                type="text"
                readOnly
                value={encodeAnchorBackup(pendingAnchor.seedHex)}
                onFocus={(e) => e.target.select()}
                aria-label="Anchor backup string"
              />
              <button className={styles.ghostBtn} type="button" onClick={handleCopyBackup}>
                {backupCopied ? 'Copied!' : 'Copy'}
              </button>
            </div>
            <p className={styles.hint}>
              Fingerprint <span className={styles.fingerprint}>{pendingAnchor.fp}</span>. Save the
              string above (password manager, printed QR) somewhere durable and offline.
            </p>
            <div className={styles.row}>
              <button className={styles.ghostBtn} type="button" onClick={() => setShowPendingQR(true)}>
                Show QR
              </button>
              <button className={styles.primaryBtn} type="button" onClick={onConfirmBackup}>
                I have saved this backup
              </button>
              <button className={styles.ghostBtn} type="button" onClick={onCancelPending}>
                Cancel
              </button>
            </div>
          </div>
        ) : (
          <>
            <p className={styles.hint}>
              No anchor in this browser yet. Generate one to found the fleet's trust root, or import
              an existing anchor backup. There is one anchor per fleet.
            </p>
            <div className={styles.row}>
              <button className={styles.primaryBtn} type="button" onClick={onGenerate}>
                Generate anchor
              </button>
            </div>
            {generateError && <p className={styles.error}>{generateError}</p>}
            <form
              className={styles.row}
              onSubmit={(e) => {
                e.preventDefault();
                onImport();
              }}
            >
              <input
                className={styles.input}
                type="text"
                value={importInput}
                onChange={(e) => setImportInput(e.target.value)}
                placeholder="bramble://anchor/v1?sk=... or 64 hex chars"
                aria-label="Anchor backup to import"
                autoComplete="off"
              />
              <button className={styles.primaryBtn} type="submit" disabled={!importInput.trim()}>
                Import anchor backup
              </button>
            </form>
            {importError && <p className={styles.error}>{importError}</p>}
          </>
        )}
      </div>

      {/* -- Node anchor status + provision -- */}
      <div className={styles.subsection}>
        <h3 className={styles.subheading}>This node</h3>
        {statusError && <p className={styles.error}>{statusError}</p>}
        {!statusError && !nodeStatus && <p className={styles.muted}>Loading anchor status...</p>}
        {nodeStatus &&
          (nodeStatus.anchored ? (
            <p className={styles.notice}>
              Anchored to <span className={styles.fingerprint}>{nodeStatus.anchor_fingerprint}</span>.{' '}
              {nodeStatus.endorsed ? 'Endorsed (enrolled).' : 'Not endorsed yet (enroll below).'}
            </p>
          ) : (
            <p className={styles.warning}>
              Not anchored. Provision the fleet anchor below before enrolling this node.
            </p>
          ))}
        {fingerprintMismatch && (
          <p className={styles.warning}>
            MISMATCH: this node is anchored to{' '}
            <span className={styles.fingerprint}>{nodeFingerprint}</span> but the anchor in this
            browser is <span className={styles.fingerprint}>{clientAnchor?.fp}</span>. A cert signed
            here would be rejected. Re-provision the anchor, or load the matching backup.
          </p>
        )}
        <div className={styles.row}>
          <button
            className={styles.primaryBtn}
            type="button"
            onClick={onProvision}
            disabled={!clientAnchor || provisioning}
            title={clientAnchor ? undefined : 'Generate or import an anchor first'}
          >
            {provisioning ? 'Provisioning...' : 'Provision anchor to this node'}
          </button>
        </div>
        {provisionError && <p className={styles.error}>{provisionError}</p>}
        {provisionSuccess && <p className={styles.notice}>{provisionSuccess}</p>}
      </div>

      {/* -- Enroll this node (local) -- */}
      <div className={styles.subsection}>
        <h3 className={styles.subheading}>Enroll this node</h3>
        <p className={styles.hint}>
          Signs a permanent endorsement over this node's identity key with the anchor held here, then
          applies it. The anchor seed never leaves this browser.
        </p>
        <div className={styles.row}>
          <button
            className={styles.primaryBtn}
            type="button"
            onClick={onEnrollLocal}
            disabled={!clientAnchor || enrolling || fingerprintMismatch}
            title={
              !clientAnchor
                ? 'Generate or import an anchor first'
                : fingerprintMismatch
                  ? 'Node is anchored to a different fingerprint'
                  : undefined
            }
          >
            {enrolling ? 'Enrolling...' : 'Enroll this node'}
          </button>
        </div>
        {enrollError && <p className={styles.error}>{enrollError}</p>}
        {enrollSuccess && <p className={styles.notice}>{enrollSuccess}</p>}
      </div>

      {/* -- Enroll a remote node (sign a cert to send back) -- */}
      <div className={styles.subsection}>
        <h3 className={styles.subheading}>Enroll a remote node</h3>
        <p className={styles.hint}>
          Paste the identity share from a node you are not connected to. This signs a permanent cert
          you send back over any channel; the remote operator applies it under "Apply an endorsement".
        </p>
        <form
          className={styles.row}
          onSubmit={(e) => {
            e.preventDefault();
            onRemoteSign();
          }}
        >
          <input
            className={styles.input}
            type="text"
            value={remoteInput}
            onChange={(e) => setRemoteInput(e.target.value)}
            placeholder="bramble://ident/v1?pk=... or 64 hex chars"
            aria-label="Remote node identity share"
            autoComplete="off"
          />
          <button className={styles.primaryBtn} type="submit" disabled={!clientAnchor || !remoteInput.trim()}>
            Sign cert
          </button>
        </form>
        {!clientAnchor && <p className={styles.hint}>Generate or import an anchor first.</p>}
        {remoteError && <p className={styles.error}>{remoteError}</p>}
        {remoteCert && (
          <>
            <div className={styles.row}>
              <span className={styles.label}>Cert</span>
              <input
                className={styles.input}
                type="text"
                readOnly
                value={remoteCert.uri}
                onFocus={(e) => e.target.select()}
                aria-label="Signed endorsement cert"
              />
              <button className={styles.ghostBtn} type="button" onClick={() => setShowRemoteQR(true)}>
                Show QR
              </button>
            </div>
            <p className={styles.hint}>
              Permanent cert for node <span className={styles.fingerprint}>{remoteCert.fp}</span>.
              Send this back to that node's operator.
            </p>
          </>
        )}
      </div>

      {/* -- Node-side affordances: show identity + apply a cert -- */}
      <div className={styles.subsection}>
        <h3 className={styles.subheading}>Get this node enrolled remotely</h3>
        <p className={styles.hint}>
          Show this node's identity to an anchor operator; apply the cert they send back.
        </p>
        <div className={styles.row}>
          <button className={styles.primaryBtn} type="button" onClick={onShowIdentity}>
            Show my identity
          </button>
        </div>
        {identityError && <p className={styles.error}>{identityError}</p>}
        {identityShare && (
          <div className={styles.row}>
            <span className={styles.label}>Identity</span>
            <input
              className={styles.input}
              type="text"
              readOnly
              value={identityShare}
              onFocus={(e) => e.target.select()}
              aria-label="This node identity share"
            />
            <button className={styles.ghostBtn} type="button" onClick={() => setShowIdentityQR(true)}>
              Show QR
            </button>
          </div>
        )}
        <form
          className={styles.row}
          onSubmit={(e) => {
            e.preventDefault();
            void onApply();
          }}
        >
          <input
            className={styles.input}
            type="text"
            value={applyInput}
            onChange={(e) => setApplyInput(e.target.value)}
            placeholder="bramble://endorse/v1?na=...&sig=..."
            aria-label="Endorsement cert to apply"
            autoComplete="off"
          />
          <button className={styles.primaryBtn} type="submit" disabled={applying || !applyInput.trim()}>
            {applying ? 'Applying...' : 'Apply endorsement'}
          </button>
        </form>
        {applyError && <p className={styles.error}>{applyError}</p>}
        {applySuccess && <p className={styles.notice}>{applySuccess}</p>}
      </div>

      {/* -- Modals -- */}
      {showPendingQR && pendingAnchor && (
        <QRShareModal
          title="Anchor backup (SECRET)"
          shareString={encodeAnchorBackup(pendingAnchor.seedHex)}
          description={`Fingerprint ${pendingAnchor.fp}. This QR carries the fleet's root secret. Store it offline; do not share it.`}
          onClose={() => setShowPendingQR(false)}
        />
      )}
      {showExport && clientAnchor && (
        <QRShareModal
          title="Anchor backup (SECRET)"
          shareString={encodeAnchorBackup(clientAnchor.seedHex)}
          description={`Fingerprint ${clientAnchor.fp}. This QR carries the fleet's root secret. Store it offline; do not share it.`}
          onClose={() => setShowExport(false)}
        />
      )}
      {showRemoteQR && remoteCert && (
        <QRShareModal
          title="Endorsement cert"
          shareString={remoteCert.uri}
          description={`Permanent cert for node ${remoteCert.fp}. Send it back to that node's operator to apply.`}
          onClose={() => setShowRemoteQR(false)}
        />
      )}
      {showIdentityQR && identityShare && (
        <QRShareModal
          title="This node's identity"
          shareString={identityShare}
          description="Public identity key. Send it to an anchor operator so they can enroll this node."
          onClose={() => setShowIdentityQR(false)}
        />
      )}
    </div>
  );
}
