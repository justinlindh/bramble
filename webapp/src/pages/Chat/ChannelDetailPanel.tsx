import { useState } from 'react';
import { useStore } from '../../store/index';
import { QRShareModal } from '../../components/QRShareModal';
import { IconLock } from '../../components/Icons';
import { encodeChannelShare } from '../../utils/channelShare';
import { removeChannel } from '../../store/actions';
import styles from './ChannelDetailPanel.module.css';

interface ChannelDetailPanelProps {
  channelIndex: number;
  onClose: () => void;
}

export function ChannelDetailPanel({ channelIndex, onClose }: ChannelDetailPanelProps) {
  const channels = useStore(s => s.config?.channels ?? []);
  const setActiveConversation = useStore(s => s.setActiveConversation);
  const ch = channels.find(c => c.index === channelIndex);

  const [showShare, setShowShare] = useState(false);
  const [shareString, setShareString] = useState('');
  const [psk, setPsk] = useState('');
  const [showPskPrompt, setShowPskPrompt] = useState(false);

  const keyEpochHelpText = 'Key epoch is the version counter for this channel encryption key. All members must be on the same epoch to decrypt messages. A mismatch usually means someone missed a key rotation and needs to resync.';

  if (!ch) {
    return (
      <div className={styles.panel}>
        <p className={styles.empty}>Channel not found.</p>
        <button className={styles.backBtn} onClick={onClose}>← Back to chat</button>
      </div>
    );
  }

  const handleShare = () => {
    if (ch.hasPsk) {
      setShowPskPrompt(true);
    } else {
      setShareString(encodeChannelShare(ch.name));
      setShowShare(true);
    }
  };

  const handlePskConfirm = () => {
    setShowPskPrompt(false);
    setShareString(encodeChannelShare(ch.name, psk.trim() || undefined));
    setShowShare(true);
    setPsk('');
  };

  const handleLeaveChannel = async () => {
    if (ch.index === 0) return;
    if (!confirm(`Leave channel "${ch.name}"?`)) return;
    await removeChannel(ch.index);
    setActiveConversation('broadcast');
    onClose();
  };

  return (
    <div className={styles.panel}>
      <button className={styles.backBtn} onClick={onClose}>← Back to chat</button>

      <div className={styles.header}>
        <h3 className={styles.name}># {ch.name}</h3>
        {ch.isDefault && <span className={styles.badge}>default</span>}
      </div>

      <div className={styles.details}>
        <div className={styles.row}>
          <span className={styles.label}>Index</span>
          <span className={styles.value}>{ch.index}</span>
        </div>
        <div className={styles.row}>
          <span className={styles.label}>Security</span>
          <span className={styles.value}>
            {ch.hasPsk ? <><IconLock size={14} /> Pre-shared key (PSK)</> : 'Open (no PSK)'}
          </span>
        </div>
        <div className={styles.row}>
          <span className={styles.labelWithHelp}>
            Key epoch
            <span
              className={styles.infoIcon}
              aria-label="Key epoch info"
              title={keyEpochHelpText}
            >
              ⓘ
            </span>
          </span>
          <span className={styles.value}>{ch.epoch}</span>
        </div>
      </div>

      <div className={styles.actions}>
        <button className={styles.shareBtn} onClick={handleShare}>
          Share Channel
        </button>
        {ch.index > 0 && (
          <button className={styles.cancelBtn} onClick={handleLeaveChannel}>
            Leave Channel
          </button>
        )}
      </div>

      {/* PSK prompt */}
      {showPskPrompt && (
        <div className={styles.pskPrompt}>
          <p className={styles.pskDesc}>
            This channel uses a PSK. Enter it to include in the share QR so others can join.
            Leave blank to share without the key.
          </p>
          <input
            type="password"
            className={styles.pskInput}
            placeholder="PSK (leave blank to omit)"
            value={psk}
            onChange={e => setPsk(e.target.value)}
            onKeyDown={e => e.key === 'Enter' && handlePskConfirm()}
            autoFocus
            autoComplete="off"
          />
          <div className={styles.pskBtns}>
            <button className={styles.shareBtn} onClick={handlePskConfirm}>Generate QR</button>
            <button className={styles.cancelBtn} onClick={() => setShowPskPrompt(false)}>Cancel</button>
          </div>
        </div>
      )}

      {/* QR Share modal */}
      {showShare && (
        <QRShareModal
          title={`Share channel "${ch.name}"`}
          shareString={shareString}
          description={
            ch.hasPsk
              ? 'Anyone with this QR or string can join — includes the PSK if you provided it.'
              : 'Anyone with this QR or string can join this open channel.'
          }
          onClose={() => setShowShare(false)}
        />
      )}
    </div>
  );
}
