import { formatAddr0x, formatAddrShort } from '../utils/address';
import { copyWithFallback } from '../utils/clipboard';
import { useTimedFlag } from '../hooks/useTimedFlag';
import styles from './AddressLabel.module.css';

interface AddressLabelProps {
  addr: number;
  name?: string;
  short?: boolean;
  className?: string;
}

export function AddressLabel({ addr, name, short = false, className }: AddressLabelProps) {
  const [copied, flashCopied] = useTimedFlag(1500);
  const [copyFailed, flashFailed] = useTimedFlag(2000);

  const fullHex = formatAddr0x(addr);
  const display = name ?? (short ? formatAddrShort(addr) : fullHex);

  const handleCopy = async (e: React.MouseEvent) => {
    e.stopPropagation();
    if (await copyWithFallback(fullHex)) {
      flashCopied();
    } else {
      flashFailed();
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
