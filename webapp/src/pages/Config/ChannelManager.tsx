import { useState } from 'react';
import type { Channel } from '../../types/bramble';
import {
  addChannel,
  removeChannel,
  setDefaultChannel,
} from '../../store/actions';
import { QRShareModal } from '../../components/QRShareModal';
import { QRScanModal } from '../../components/QRScanModal';
import { EscapeDialog } from '../../components/EscapeDialog';
import { IconLock } from '../../components/Icons';
import type { ScanResult } from '../../components/QRScanModal';
import { encodeChannelShare } from '../../utils/channelShare';
import { clampToUtf8Bytes, utf8Length, CHANNEL_NAME_BUDGET_BYTES } from '../../utils/byteLimit';
import { friendlyErrorFrom } from '../../lib/errors';
import styles from './ChannelManager.module.css';

interface ChannelManagerProps {
  channels: Channel[];
}

// Shown when user clicks Share on a PSK-protected channel
function PskPromptModal({
  channelName,
  onConfirm,
  onClose,
}: {
  channelName: string;
  onConfirm: (psk: string) => void;
  onClose: () => void;
}) {
  const [psk, setPsk] = useState('');
  const handleSubmit = (e: React.FormEvent) => {
    e.preventDefault();
    onConfirm(psk.trim());
  };
  return (
    <EscapeDialog
      ariaLabel={`Share "${channelName}"`}
      onClose={onClose}
      backdropClassName={styles.backdrop}
      dialogClassName={styles.promptModal}
    >
      <button className={styles.promptClose} onClick={onClose} aria-label="Close">✕</button>
      <h3 className={styles.promptTitle}>Share "{channelName}"</h3>
      <p className={styles.promptDesc}>
        This channel has a PSK. Enter it to include in the share QR so others
        can join. Leave blank to share without the key.
      </p>
      <form className={styles.promptForm} onSubmit={handleSubmit}>
        <input
          type="password"
          className={styles.promptInput}
          placeholder="PSK (leave blank to omit)"
          value={psk}
          onChange={(e) => setPsk(e.target.value)}
          autoFocus
          autoComplete="off"
          aria-label="Channel PSK for sharing"
        />
        <button type="submit" className={styles.promptBtn}>
          Generate QR
        </button>
      </form>
    </EscapeDialog>
  );
}

export function ChannelManager({ channels }: ChannelManagerProps) {
  const [newName, setNewName] = useState('');
  const [newPsk, setNewPsk] = useState('');
  const [adding, setAdding] = useState(false);
  const [error, setError] = useState('');

  // In-app confirmation state (replaces native confirm())
  const [confirmAction, setConfirmAction] = useState<{
    label: string;
    onConfirm: () => void;
  } | null>(null);

  // Share state
  const [shareChannel, setShareChannel] = useState<Channel | null>(null);
  const [shareString, setShareString] = useState('');
  const [showShare, setShowShare] = useState(false);

  // PSK prompt (when sharing a channel that has a PSK)
  const [pskPromptChannel, setPskPromptChannel] = useState<Channel | null>(null);

  // Import / scan state
  const [showScan, setShowScan] = useState(false);
  const [importSuccess, setImportSuccess] = useState('');

  // ── Add channel ──────────────────────────────────────────────────────────
  const handleAdd = async (e: React.FormEvent) => {
    e.preventDefault();
    const name = newName.trim();
    if (!name) return;
    setAdding(true);
    setError('');
    try {
      await addChannel(name, newPsk.trim() || undefined);
      setNewName('');
      setNewPsk('');
    } catch (err) {
      setError(friendlyErrorFrom(err));
    } finally {
      setAdding(false);
    }
  };

  // ── Remove channel ────────────────────────────────────────────────────────
  const handleRemove = (index: number, name: string) => {
    setConfirmAction({
      label: `Remove channel "${name}"?`,
      onConfirm: async () => {
        setConfirmAction(null);
        setError('');
        try {
          await removeChannel(index);
        } catch (err) {
          setError(friendlyErrorFrom(err));
        }
      },
    });
  };

  // ── Set default ───────────────────────────────────────────────────────────
  const handleSetDefault = (index: number, name: string) => {
    setConfirmAction({
      label: `Set "${name}" as default channel?`,
      onConfirm: async () => {
        setConfirmAction(null);
        setError('');
        try {
          await setDefaultChannel(index);
        } catch (err) {
          setError(friendlyErrorFrom(err));
        }
      },
    });
  };

  // ── Share channel ─────────────────────────────────────────────────────────
  const handleShare = (ch: Channel) => {
    if (ch.hasPsk) {
      // Ask the user to re-enter the PSK (server doesn't send it back)
      setPskPromptChannel(ch);
    } else {
      setShareChannel(ch);
      setShareString(encodeChannelShare(ch.name));
      setShowShare(true);
    }
  };

  const handlePskConfirm = (psk: string) => {
    if (!pskPromptChannel) return;
    setPskPromptChannel(null);
    setShareChannel(pskPromptChannel);
    setShareString(encodeChannelShare(pskPromptChannel.name, psk || undefined));
    setShowShare(true);
  };

  // ── Import from QR / string ───────────────────────────────────────────────
  const handleScanResult = async (result: ScanResult) => {
    setShowScan(false);
    if (result.kind !== 'channel') {
      setError('Scanned a node share, not a channel share.');
      return;
    }
    const { name, psk } = result.data;
    setError('');
    try {
      await addChannel(name, psk || undefined);
      setImportSuccess(`Channel "${name}" added!`);
      setTimeout(() => setImportSuccess(''), 3000);
    } catch (err) {
      setError(friendlyErrorFrom(err));
    }
  };

  return (
    <div>
      {/* ── Channel list ── */}
      <div className={styles.list}>
        {channels.length === 0 && (
          <p style={{ color: 'var(--text-muted)', fontSize: '0.875rem' }}>
            No channels configured.
          </p>
        )}
        {channels.map((ch) => (
          <div key={ch.index} className={styles.channelRow}>
            <span className={styles.idx}>#{ch.index}</span>
            <span className={styles.chName}>{ch.name}</span>
            {ch.isDefault && (
              <span className={`${styles.badge} ${styles.badgeDefault}`}>
                default
              </span>
            )}
            {ch.hasPsk && (
              <span className={`${styles.badge} ${styles.badgePsk}`}>
                <IconLock size={12} /> PSK
              </span>
            )}
            <span className={styles.epoch} title="Key rotation epoch">
              epoch {ch.epoch}
            </span>
            <div className={styles.rowActions}>
              {/* Share button */}
              <button
                className={styles.shareBtn}
                onClick={() => handleShare(ch)}
                title={`Share channel "${ch.name}" as QR code`}
              >
                ⬆ Share
              </button>
              {!ch.isDefault && (
                <button
                  className={styles.setDefaultBtn}
                  onClick={() => handleSetDefault(ch.index, ch.name)}
                  title="Set as default channel"
                >
                  Set default
                </button>
              )}
              <button
                className={styles.removeBtn}
                onClick={() => handleRemove(ch.index, ch.name)}
                title={`Remove channel ${ch.name}`}
                disabled={ch.isDefault}
              >
                ✕
              </button>
            </div>
          </div>
        ))}
      </div>

      {/* ── Add channel form ── */}
      <form className={styles.addForm} onSubmit={handleAdd}>
        <div className={styles.nameInputWrap}>
          <input
            className={styles.addInput}
            type="text"
            placeholder="Channel name"
            value={newName}
            /* handle_add_channel measures the name with strlen, so the budget
               is bytes. maxLength would count UTF-16 units and let a
               non-ASCII name past the client into a -32602. */
            onChange={(e) => setNewName(clampToUtf8Bytes(e.target.value, CHANNEL_NAME_BUDGET_BYTES))}
            aria-label="New channel name"
          />
          {newName.length > 0 && (
            <span className={styles.charCount}>{utf8Length(newName)}/{CHANNEL_NAME_BUDGET_BYTES}</span>
          )}
        </div>
        <input
          className={styles.pskInput}
          type="password"
          placeholder="PSK (optional)"
          value={newPsk}
          onChange={(e) => setNewPsk(e.target.value)}
          aria-label="Channel pre-shared key"
          autoComplete="new-password"
        />
        <button
          className={styles.addBtn}
          type="submit"
          disabled={adding || !newName.trim()}
        >
          {adding ? 'Adding…' : '+ Add'}
        </button>
        {/* Import from QR / string */}
        <button
          type="button"
          className={styles.importBtn}
          onClick={() => { setError(''); setImportSuccess(''); setShowScan(true); }}
          title="Import channel from QR code or share string"
        >
          ⬇ Import
        </button>
        {error && <span className={styles.error}>{error}</span>}
        {importSuccess && <span className={styles.success}>{importSuccess}</span>}
      </form>

      {/* ── PSK prompt modal ── */}
      {pskPromptChannel && (
        <PskPromptModal
          channelName={pskPromptChannel.name}
          onConfirm={handlePskConfirm}
          onClose={() => setPskPromptChannel(null)}
        />
      )}

      {/* ── QR share modal ── */}
      {showShare && shareChannel && (
        <QRShareModal
          title={`Share channel "${shareChannel.name}"`}
          shareString={shareString}
          description={
            shareChannel.hasPsk
              ? 'Anyone with this QR or string can join the channel, which includes the PSK if you provided it.'
              : 'Anyone with this QR or string can join this public channel.'
          }
          onClose={() => { setShowShare(false); setShareChannel(null); }}
        />
      )}

      {/* ── QR scan / import modal ── */}
      {showScan && (
        <QRScanModal
          onResult={handleScanResult}
          onClose={() => setShowScan(false)}
        />
      )}

      {/* ── In-app confirmation dialog ── */}
      {confirmAction && (
        <EscapeDialog
          ariaLabel={confirmAction.label}
          onClose={() => setConfirmAction(null)}
          backdropClassName={styles.backdrop}
          dialogClassName={styles.promptModal}
        >
          <p className={styles.promptTitle}>{confirmAction.label}</p>
          <div className={styles.confirmActions}>
            <button
              className={styles.confirmBtn}
              onClick={confirmAction.onConfirm}
              autoFocus
            >
              Confirm
            </button>
            <button
              className={styles.cancelBtn}
              onClick={() => setConfirmAction(null)}
            >
              Cancel
            </button>
          </div>
        </EscapeDialog>
      )}
    </div>
  );
}
