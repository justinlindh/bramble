import { useState } from 'react';
import { formatAddr0x, formatAddrShort } from '../utils/address';
import styles from './AddressLabel.module.css';

interface AddressLabelProps {
  addr: number;
  name?: string;
  short?: boolean;
  className?: string;
}

export function AddressLabel({ addr, name, short = false, className }: AddressLabelProps) {
  const [copied, setCopied] = useState(false);

  const fullHex = formatAddr0x(addr);
  const display = name ?? (short ? formatAddrShort(addr) : fullHex);

  const [copyFailed, setCopyFailed] = useState(false);

  const handleCopy = async (e: React.MouseEvent) => {
    e.stopPropagation();
    try {
      await navigator.clipboard.writeText(fullHex);
      setCopied(true);
      setTimeout(() => setCopied(false), 1500);
    } catch {
      // Fallback for contexts where clipboard API is denied
      try {
        const ta = document.createElement('textarea');
        ta.value = fullHex;
        ta.style.position = 'fixed';
        ta.style.opacity = '0';
        document.body.appendChild(ta);
        ta.select();
        document.execCommand('copy');
        document.body.removeChild(ta);
        setCopied(true);
        setTimeout(() => setCopied(false), 1500);
      } catch {
        setCopyFailed(true);
        setTimeout(() => setCopyFailed(false), 2000);
      }
    }
  };

  return (
    <span className={`${styles.label} ${className ?? ''}`} title={fullHex}>
      <span className={styles.mono}>{display}</span>
      <button
        className={styles.copy}
        onClick={handleCopy}
        aria-label={`Copy address ${fullHex}`}
        title={copyFailed ? 'Copy failed' : copied ? 'Copied!' : 'Copy address'}
      >
        {copyFailed ? '✗' : copied ? '✓' : '⧉'}
      </button>
    </span>
  );
}
