import { useEffect, useRef, useState } from 'react';
import QRCode from 'qrcode';
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

  // Render QR code into canvas
  useEffect(() => {
    if (!canvasRef.current) return;
    QRCode.toCanvas(canvasRef.current, shareString, {
      width: 260,
      margin: 2,
      color: {
        dark: '#e6edf3',
        light: '#161b22',
      },
    }).catch(console.error);
  }, [shareString]);

  const handleCopy = async () => {
    try {
      await navigator.clipboard.writeText(shareString);
      setCopied(true);
      setTimeout(() => setCopied(false), 2000);
    } catch {
      // Fallback: select the text field
      const el = document.getElementById('qr-share-string') as HTMLInputElement;
      el?.select();
    }
  };

  // Close on backdrop click
  const handleBackdropClick = (e: React.MouseEvent<HTMLDivElement>) => {
    if (e.target === e.currentTarget) onClose();
  };

  return (
    <div className={styles.backdrop} onClick={handleBackdropClick} role="dialog" aria-modal aria-label={title}>
      <div className={styles.modal}>
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
      </div>
    </div>
  );
}
