import { useState, useRef, useEffect } from 'react';
import { IconLocation } from '../../components/Icons';
import { shareLocationOnce } from '../../store/actions';
import type { LocationTier } from '../../types/bramble';
import styles from './ShareLocationButton.module.css';

interface ShareLocationButtonProps {
  dest: number;
}

export function ShareLocationButton({ dest }: ShareLocationButtonProps) {
  const [open, setOpen] = useState(false);
  const [sending, setSending] = useState(false);
  const wrapperRef = useRef<HTMLDivElement>(null);

  // Close dropdown on outside click
  useEffect(() => {
    if (!open) return;
    const handler = (e: MouseEvent) => {
      if (wrapperRef.current && !wrapperRef.current.contains(e.target as Node)) {
        setOpen(false);
      }
    };
    document.addEventListener('mousedown', handler);
    return () => document.removeEventListener('mousedown', handler);
  }, [open]);

  const handleShare = async (tier: LocationTier) => {
    setSending(true);
    setOpen(false);
    try {
      await shareLocationOnce(dest, tier);
    } finally {
      setSending(false);
    }
  };

  return (
    <div className={styles.wrapper} ref={wrapperRef}>
      <button
        className={styles.trigger}
        onClick={() => setOpen((v) => !v)}
        disabled={sending}
        title="Share location"
        aria-label="Share location"
      >
        <IconLocation size={16} />
      </button>

      {open && (
        <div className={styles.dropdown}>
          <button
            className={styles.option}
            onClick={() => handleShare('coarse')}
            disabled={sending}
          >
            📍 Zone (~1 km)
          </button>
          <button
            className={styles.option}
            onClick={() => handleShare('full')}
            disabled={sending}
          >
            🎯 Exact position
          </button>
        </div>
      )}
    </div>
  );
}
