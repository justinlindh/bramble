import { useStore } from '../../store/index';
import styles from './Config.module.css';

export function Config() {
  const config = useStore(s => s.config);

  if (!config) {
    return (
      <div className={styles.config}>
        <p className={styles.empty}>Connect to a node to view configuration.</p>
      </div>
    );
  }

  return (
    <div className={styles.config}>
      <section className={styles.section}>
        <h2>Identity</h2>
        <div className={styles.row}>
          <span className={styles.label}>Address</span>
          <span className={styles.mono}>0x{config.identity.address.toString(16).toUpperCase().padStart(8, '0')}</span>
        </div>
        <div className={styles.row}>
          <span className={styles.label}>Name</span>
          <span>{config.identity.name}</span>
        </div>
      </section>

      <section className={styles.section}>
        <h2>Radio</h2>
        <div className={styles.row}>
          <span className={styles.label}>TX Power</span>
          <span>{config.radio.txPowerDbm} dBm</span>
        </div>
        <div className={styles.row}>
          <span className={styles.label}>Spreading Factor</span>
          <span>SF{config.radio.sf}</span>
        </div>
        <div className={styles.row}>
          <span className={styles.label}>Bandwidth</span>
          <span>{config.radio.bwKhz} kHz</span>
        </div>
        <div className={styles.row}>
          <span className={styles.label}>Frequency</span>
          <span>{config.radio.freqMhz} MHz</span>
        </div>
      </section>

      <section className={styles.section}>
        <h2>Channels ({config.channels.length})</h2>
        {config.channels.map(ch => (
          <div key={ch.index} className={styles.row}>
            <span className={styles.mono}>#{ch.index}</span>
            <span>{ch.name}</span>
            {ch.isDefault && <span className={styles.badge}>default</span>}
            {ch.hasPsk && <span className={styles.badge}>🔒</span>}
          </div>
        ))}
      </section>
    </div>
  );
}
