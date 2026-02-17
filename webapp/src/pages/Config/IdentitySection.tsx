import { useState, useEffect } from 'react';
import type { NodeIdentity } from '../../types/bramble';
import { saveNodeName, setMailbox } from '../../store/actions';
import { useStore } from '../../store/index';
import styles from './IdentitySection.module.css';

interface IdentitySectionProps {
  identity: NodeIdentity;
}

export function IdentitySection({ identity }: IdentitySectionProps) {
  const [name, setName] = useState(identity.name);

  // Sync local name state when identity prop changes (e.g. after node truncates to 8 chars)
  useEffect(() => {
    setName(identity.name);
  }, [identity.name]);
  const [saving, setSaving] = useState(false);
  const [saved, setSaved] = useState(false);
  const [error, setError] = useState('');

  const handleSaveName = async (e: React.FormEvent) => {
    e.preventDefault();
    setSaving(true);
    setError('');
    setSaved(false);
    try {
      await saveNodeName(name.trim().slice(0, 8));
      setSaved(true);
      setTimeout(() => setSaved(false), 2000);
    } catch (err) {
      setError((err as Error).message);
    } finally {
      setSaving(false);
    }
  };

  const handleExport = async () => {
    try {
      const backup = {
        version: 1,
        address: `0x${identity.address.toString(16).toUpperCase().padStart(8, '0')}`,
        pubkeyHash: `0x${identity.pubkeyHash.toString(16).toUpperCase().padStart(8, '0')}`,
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
      setError('Export failed: ' + (err as Error).message);
    }
  };

  const mailboxEnabled = useStore(s => s.config?.mailboxEnabled ?? false);

  const addrHex = `0x${identity.address.toString(16).toUpperCase().padStart(8, '0')}`;
  const pubkeyHashHex = `0x${identity.pubkeyHash.toString(16).toUpperCase().padStart(8, '0')}`;

  return (
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
            maxLength={8}
            placeholder="Up to 8 chars"
            onChange={(e) => setName(e.target.value)}
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
                setError((err as Error).message);
              }
            }}
          />
          <span>{mailboxEnabled ? 'Enabled' : 'Disabled'}</span>
        </label>
      </div>

      {/* Export */}
      <div className={styles.row}>
        <span className={styles.label} />
        <button className={styles.exportBtn} onClick={handleExport}>
          🔑 Export Key Backup
        </button>
      </div>
    </div>
  );
}
