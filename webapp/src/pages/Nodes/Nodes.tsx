import { useStore } from '../../store/index';
import styles from './Nodes.module.css';

export function Nodes() {
  const neighbors = useStore(s => s.neighbors);
  const routes = useStore(s => s.routes);

  return (
    <div className={styles.nodes}>
      <h2>📡 Neighbors ({neighbors.length})</h2>
      {neighbors.length === 0 && (
        <p className={styles.empty}>No neighbors discovered yet.</p>
      )}

      <h2 className={styles.sectionTitle}>🗺 Routes ({routes.length})</h2>
      {routes.length === 0 && (
        <p className={styles.empty}>No routes in table.</p>
      )}
    </div>
  );
}
