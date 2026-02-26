import { useState } from 'react';
import styles from './AddressLabel.module.css';

interface AddressLabelProps {
  addr: number;
  name?: string;
  short?: boolean;
  className?: string;
}

function fmtAddr(addr: number, short: boolean): string {
  const hex = addr.toString(16).toUpperCase().padStart(8, '0');
  if (short) return `0x${hex.slice(-4)}`;
  return `0x${hex}`;
}

export function AddressLabel({ addr, name, short = false, className }: AddressLabelProps) {
  const [copied, setCopied] = useState(false);

  const fullHex = `0x${addr.toString(16).toUpperCase().padStart(8, '0')}`;
  const display = name ?? fmtAddr(addr, short);

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
