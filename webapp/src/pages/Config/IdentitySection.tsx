import { useState, useEffect } from 'react';
import type { NodeIdentity } from '../../types/bramble';
import { saveNodeName, setMailbox } from '../../store/actions';
import { useStore } from '../../store/index';
import { QRShareModal } from '../../components/QRShareModal';
import { encodeNodeShare } from '../../utils/channelShare';
import { formatAddr0x } from '../../utils/address';
import { clampToUtf8Bytes, NODE_NAME_MAX_BYTES } from '../../utils/byteLimit';
import { IconKey, IconNodes } from '../../components/Icons';
import { useTimedFlag } from '../../hooks/useTimedFlag';
import { friendlyErrorFrom } from '../../lib/errors';
import styles from './IdentitySection.module.css';

interface IdentitySectionProps {
  identity: NodeIdentity;
}

export function IdentitySection({ identity }: IdentitySectionProps) {
  const [name, setName] = useState(identity.name);

  // Sync local name state when identity prop changes (e.g. after node applies validation/truncation)
  useEffect(() => {
    setName(identity.name);
  }, [identity.name]);
  const [saving, setSaving] = useState(false);
  const [saved, flashSaved, resetSaved] = useTimedFlag(2000);
  const [error, setError] = useState('');
  const [showNodeShare, setShowNodeShare] = useState(false);

  const handleSaveName = async (e: React.FormEvent) => {
    e.preventDefault();
    setSaving(true);
    setError('');
    resetSaved();
    try {
      // Trimming can only shorten, but the bound still has to be re-applied
      // in bytes: slice(0, 32) counts UTF-16 units and can cut a surrogate
      // pair in half, which sends a lone surrogate to the node.
      await saveNodeName(clampToUtf8Bytes(name.trim(), NODE_NAME_MAX_BYTES));
      flashSaved();
    } catch (err) {
      setError(friendlyErrorFrom(err));
    } finally {
      setSaving(false);
    }
  };

  const handleExport = async () => {
    try {
      const backup = {
        version: 1,
        address: formatAddr0x(identity.address),
        pubkeyHash: formatAddr0x(identity.pubkeyHash),
        pubkey: identity.pubkeyB64,
        name: identity.name,
        exportedAt: new Date().toISOString(),
      };
      const blob = new Blob([JSON.stringify(backup, null, 2)], { type: 'application/json' });
      const url = URL.createObjectURL(blob);
      const a = document.createElement('a');
      a.href = url;
      a.download = `bramble-backup-${identity.address.toString(16).padStart(8, '0')}.bramble-backup`;
      a.click();
      URL.revokeObjectURL(url);
    } catch (err) {
      setError('Export failed: ' + friendlyErrorFrom(err));
    }
  };

  const mailboxEnabled = useStore(s => s.config?.mailboxEnabled ?? false);

  const addrHex = formatAddr0x(identity.address);
  const pubkeyHashHex = formatAddr0x(identity.pubkeyHash);

  return (
    <>
    <div className={styles.section}>
      {/* Address */}
      <div className={styles.row}>
        <span className={styles.label}>Address</span>
        <span className={styles.mono}>{addrHex}</span>
      </div>

      {/* Public key hash */}
      <div className={styles.row}>
        <span className={styles.label}>Key hash</span>
        <span
          className={styles.pubkeyHash}
          title={identity.pubkeyB64 ? `Pubkey: ${identity.pubkeyB64}` : pubkeyHashHex}
        >
          {pubkeyHashHex}
        </span>
      </div>

      {/* Editable name */}
      <div className={styles.row}>
        <span className={styles.label}>Name</span>
        <form className={styles.nameForm} onSubmit={handleSaveName}>
          <input
            className={styles.nameInput}
            type="text"
            value={name}
            placeholder="Up to 32 bytes"
            /* bramble.setNodeName rejects a name over BRAMBLE_NODE_NAME_MAX
               bytes, measured with strlen, so clamp on bytes rather than on
               the UTF-16 units maxLength would count. */
            onChange={(e) => setName(clampToUtf8Bytes(e.target.value, NODE_NAME_MAX_BYTES))}
            aria-label="Node name"
          />
          <button className={styles.saveBtn} type="submit" disabled={saving}>
            {saving ? 'Saving…' : 'Save'}
          </button>
          {saved && <span className={styles.savedMsg}>✓ Saved</span>}
          {error && <span className={styles.error}>{error}</span>}
        </form>
      </div>

      {/* Mailbox toggle */}
      <div className={styles.row}>
        <span className={styles.label}>Mailbox</span>
        <label className={styles.toggle} title="When enabled, this node stores messages for offline destinations and delivers them when they come back in range">
          <input
            type="checkbox"
            checked={mailboxEnabled}
            onChange={async (e) => {
              try {
                await setMailbox(e.target.checked);
              } catch (err) {
                setError(friendlyErrorFrom(err));
              }
            }}
          />
          <span>{mailboxEnabled ? 'Enabled' : 'Disabled'}</span>
        </label>
      </div>

      {/* Export */}
      <div className={styles.row}>
        <span className={styles.label} />
        <div style={{ display: 'flex', gap: '0.5rem', flexWrap: 'wrap' }}>
          <button className={styles.exportBtn} onClick={handleExport}>
            <IconKey size={16} /> Export Key Backup
          </button>
          <button
            className={styles.exportBtn}
            onClick={() => setShowNodeShare(true)}
            title="Share your node identity as a QR code so others can save your public key"
          >
            <IconNodes size={16} /> Share Node
          </button>
        </div>
      </div>
    </div>

    {/* ── Node identity share modal ── */}
    {showNodeShare && (
      <QRShareModal
        title={`Share node "${identity.name || addrHex}"`}
        shareString={encodeNodeShare(identity.name, identity.address, identity.pubkeyB64)}
        description="Share this QR so others can save your public key and verify your identity. Does not expose private keys."
        onClose={() => setShowNodeShare(false)}
      />
    )}
  </>
  );
}
