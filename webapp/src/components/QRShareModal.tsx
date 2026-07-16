import { useEffect, useRef, useState } from 'react';
import { EscapeDialog } from './EscapeDialog';
import styles from './QRShareModal.module.css';

interface QRShareModalProps {
  title: string;
  shareString: string;
  /** Optional description rendered beneath the title */
  description?: string;
  onClose: () => void;
}

export function QRShareModal({
  title,
  shareString,
  description,
  onClose,
}: QRShareModalProps) {
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const [copied, setCopied] = useState(false);
  const [copyError, setCopyError] = useState<string | null>(null);

  // Render QR code into canvas. qrcode is loaded on demand to keep it out of
  // the main bundle; it is only needed when this modal is actually shown.
  useEffect(() => {
    if (!canvasRef.current) return;
    const canvas = canvasRef.current;
    import('qrcode').then(({ default: QRCode }) => {
      QRCode.toCanvas(canvas, shareString, {
        width: 260,
        margin: 2,
        color: {
          dark: '#e6edf3',
          light: '#161b22',
        },
      }).catch(console.error);
    }).catch(console.error);
  }, [shareString]);

  const handleCopy = async () => {
    setCopyError(null);
    try {
      await navigator.clipboard.writeText(shareString);
      setCopied(true);
      setTimeout(() => setCopied(false), 2000);
      return;
    } catch {
      // Fallback for older/locked-down clipboard APIs.
      const el = document.getElementById('qr-share-string') as HTMLInputElement | null;
      if (el) {
        el.focus();
        el.select();
        try {
          const ok = document.execCommand('copy');
          if (ok) {
            setCopied(true);
            setTimeout(() => setCopied(false), 2000);
            return;
          }
        } catch {
          // handled below
        }
      }
      setCopyError('Could not copy automatically. Please select and copy manually.');
    }
  };

  return (
    <EscapeDialog
      ariaLabel={title}
      onClose={onClose}
      backdropClassName={styles.backdrop}
      dialogClassName={styles.modal}
    >
      <button className={styles.closeBtn} onClick={onClose} aria-label="Close">✕</button>

      <h3 className={styles.title}>{title}</h3>
      {description && <p className={styles.description}>{description}</p>}

      {/* QR code */}
      <div className={styles.qrWrap}>
        <canvas ref={canvasRef} className={styles.qrCanvas} />
      </div>

      {/* Copyable string */}
      <div className={styles.stringRow}>
        <input
          id="qr-share-string"
          className={styles.stringInput}
          type="text"
          readOnly
          value={shareString}
          onFocus={(e) => e.target.select()}
          aria-label="Share string"
        />
        <button className={styles.copyBtn} onClick={handleCopy}>
          {copied ? '✓ Copied!' : 'Copy'}
        </button>
      </div>

      <p className={styles.hint}>
        Scan the QR code or share the text string to add this on another device.
      </p>
      {copyError && (
        <p className={styles.hint} role="status" aria-live="polite">
          {copyError}
        </p>
      )}
    </EscapeDialog>
  );
}
