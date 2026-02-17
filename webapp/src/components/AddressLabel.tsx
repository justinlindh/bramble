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

  const handleCopy = async (e: React.MouseEvent) => {
    e.stopPropagation();
    await navigator.clipboard.writeText(fullHex);
    setCopied(true);
    setTimeout(() => setCopied(false), 1500);
  };

  return (
    <span className={`${styles.label} ${className ?? ''}`} title={fullHex}>
      <span className={styles.mono}>{display}</span>
      <button
        className={styles.copy}
        onClick={handleCopy}
        aria-label={`Copy address ${fullHex}`}
        title={copied ? 'Copied!' : 'Copy address'}
      >
        {copied ? '✓' : '⧉'}
      </button>
    </span>
  );
}
