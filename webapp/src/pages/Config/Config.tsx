import { useStore } from '../../store/index';
import { IdentitySection } from './IdentitySection';
import { RadioForm } from './RadioForm';
import { ChannelManager } from './ChannelManager';
import { PeerManager } from './PeerManager';
import styles from './Config.module.css';

export function Config() {
  const config = useStore((s) => s.config);
  const neighbors = useStore((s) => s.neighbors);
  const routes = useStore((s) => s.routes);

  if (!config) {
    return (
      <div className={styles.config}>
        <p className={styles.empty}>Connect to a node to view configuration.</p>
      </div>
    );
  }

  return (
    <div className={styles.config}>
      {/* ── Node Identity ── */}
      <section className={styles.section}>
        <h2>🪪 Identity</h2>
        <IdentitySection identity={config.identity} />
      </section>

      {/* ── Radio Settings ── */}
      <section className={styles.section}>
        <h2>📻 Radio Settings</h2>
        <RadioForm radio={config.radio} />
      </section>

      {/* ── Channel Manager ── */}
      <section className={styles.section}>
        <h2>📡 Channels ({config.channels.length})</h2>
        <ChannelManager channels={config.channels} />
      </section>

      {/* ── Peer Manager ── */}
      <section className={styles.section}>
        <h2>👥 Peers</h2>
        <PeerManager neighbors={neighbors} routes={routes} />
      </section>
    </div>
  );
}
