import { useStore } from '../../store/index';
import { loadNeighbors, loadRoutes, openDM } from '../../store/actions';
import { usePoll } from '../../hooks/usePoll';
import { NeighborCard } from './NeighborCard';
import { RouteTable } from './RouteTable';
import { IconNodes, IconRoutes } from '../../components/Icons';
import styles from './Nodes.module.css';

export function Nodes() {
  const neighbors = useStore((s) => s.neighbors);
  const routes = useStore((s) => s.routes);
  const connected = useStore((s) => s.connectionState === 'connected');

  // Auto-refresh: neighbors every 5s, routes every 10s
  usePoll(loadNeighbors, 5000);
  usePoll(loadRoutes, 10000);

  return (
    <div className={styles.nodes}>
      {/* ── Neighbor cards ── */}
      <header className={styles.sectionHeader}>
        <h2><IconNodes size={18} /> Neighbors</h2>
        <span className={styles.count}>{neighbors.length}</span>
        {!connected && (
          <span className={styles.offlinePill}>offline</span>
        )}
      </header>

      {neighbors.length === 0 ? (
        <p className={styles.empty}>
          {connected
            ? 'No neighbors discovered yet. Are other Bramble nodes nearby?'
            : 'Connect to a node to see neighbors.'}
        </p>
      ) : (
        <div className={styles.cardGrid}>
          {neighbors.map((n) => (
            <NeighborCard key={n.addr} neighbor={n} onOpenDM={openDM} />
          ))}
        </div>
      )}

      {/* ── Route table ── */}
      <header className={styles.sectionHeader} style={{ marginTop: '1rem' }}>
        <h2><IconRoutes size={18} /> Routes</h2>
        <span className={styles.count}>{routes.length}</span>
      </header>

      <RouteTable routes={routes} />
    </div>
  );
}
