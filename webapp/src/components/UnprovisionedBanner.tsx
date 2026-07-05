import { useStore } from '../store/index';
import styles from './UnprovisionedBanner.module.css';

/**
 * Top-level, always-visible warning that this node is UNPROVISIONED and
 * therefore INERT: it has no network key, so it is not meshing (it will not
 * originate or verify authenticated control-plane traffic) until an operator
 * provisions a key. A silently-inert node is confusing, so this is a prominent
 * banner across the app shell rather than a buried config row. Clicking it
 * jumps to the Config tab where the key can be generated or pasted.
 *
 * Rendered only when connected and the polled status says provisioned === false;
 * it disappears the moment a key is set.
 */
export function UnprovisionedBanner() {
  const status = useStore((s) => s.networkKeyStatus);
  const connectionState = useStore((s) => s.connectionState);
  const setActiveTab = useStore((s) => s.setActiveTab);

  if (connectionState !== 'connected') return null;
  if (!status || status.provisioned) return null;

  return (
    <div className={styles.banner} role="alert">
      <span className={styles.icon} aria-hidden="true">
        &#9888;
      </span>
      <span className={styles.text}>
        <strong className={styles.title}>This node is UNPROVISIONED and inert.</strong>{' '}
        It has no network key, so it is not meshing. Generate a key here to found a
        network, or paste one from an existing node to join.
      </span>
      <button
        className={styles.action}
        type="button"
        onClick={() => setActiveTab('config')}
      >
        Provision
      </button>
    </div>
  );
}
