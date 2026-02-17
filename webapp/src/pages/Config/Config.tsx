import { useStore } from '../../store/index';
import { IdentitySection } from './IdentitySection';
import { RadioForm } from './RadioForm';
import { ChannelManager } from './ChannelManager';
import { PeerManager } from './PeerManager';
import { LocationSection } from './LocationSection';
import { IconIdentity, IconRadio, IconNodes, IconPeers, IconLocation } from '../../components/Icons';
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
        <h2><IconIdentity size={18} /> Identity</h2>
        <IdentitySection identity={config.identity} />
      </section>

      {/* ── Radio Settings ── */}
      <section className={styles.section}>
        <h2><IconRadio size={18} /> Radio Settings</h2>
        <RadioForm radio={config.radio} />
      </section>

      {/* ── Channel Manager ── */}
      <section className={styles.section}>
        <h2><IconNodes size={18} /> Channels ({config.channels.length})</h2>
        <ChannelManager channels={config.channels} />
      </section>

      {/* ── Peer Manager ── */}
      <section className={styles.section}>
        <h2><IconPeers size={18} /> Peers</h2>
        <PeerManager neighbors={neighbors} routes={routes} />
      </section>

      {/* ── Location ── */}
      <section className={styles.section}>
        <h2><IconLocation size={18} /> Location</h2>
        <LocationSection location={config.location} neighbors={neighbors} />
      </section>
    </div>
  );
}
