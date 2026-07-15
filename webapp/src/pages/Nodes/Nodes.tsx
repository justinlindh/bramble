import { useStore } from '../../store/index';
import { loadNeighbors, loadRoutes, loadPeerLocations, openDM, showOnMap } from '../../store/actions';
import { resolvePeerName } from '../../store/peerName';
import { usePoll } from '../../hooks/usePoll';
import { NeighborCard } from './NeighborCard';
import { RouteTable } from './RouteTable';
import { IconNodes, IconRoutes } from '../../components/Icons';
import styles from './Nodes.module.css';
import { buildKnownPeers } from './knownPeers';

function formatAddr(addr: number): string {
  return `0x${addr.toString(16).toUpperCase().padStart(8, '0')}`;
}

export function Nodes() {
  const neighbors = useStore((s) => s.neighbors);
  const routes = useStore((s) => s.routes);
  const peerLocations = useStore((s) => s.peerLocations);
  const peerNames = useStore((s) => s.peerNames);
  const connected = useStore((s) => s.connectionState === 'connected');
  // undefined = never fetched since connect; [] = fetched, no neighbors found.
  const neighborsLoading = connected && neighbors === undefined;
  const neighborList = neighbors ?? [];
  const knownPeers = buildKnownPeers(neighborList, routes, peerLocations);

  // Auto-refresh: neighbors every 5s, routes every 10s, peer locations every 10s
  usePoll(loadNeighbors, 5000);
  usePoll(loadRoutes, 10000);
  usePoll(loadPeerLocations, 10000);

  return (
    <div className={styles.nodes}>
      {/* ── Neighbor cards ── */}
      <header className={styles.sectionHeader}>
        <h2><IconNodes size={18} /> Neighbors</h2>
        <span className={styles.count}>{neighborList.length}</span>
        {!connected && (
          <span className={styles.offlinePill}>offline</span>
        )}
      </header>

      {neighborsLoading ? (
        <p className={styles.empty}>Loading neighbors…</p>
      ) : neighborList.length === 0 ? (
        <p className={styles.empty}>
          {connected
            ? 'No direct radio neighbors discovered yet.'
            : 'Connect to a node to see neighbors.'}
        </p>
      ) : (
        <div className={styles.cardGrid}>
          {neighborList.map((n) => (
            <NeighborCard key={n.addr} neighbor={n} peerLocation={peerLocations.find(l => l.addr === n.addr)} onOpenDM={openDM} onShowOnMap={showOnMap} />
          ))}
        </div>
      )}

      {connected && knownPeers.length > 0 && (
        <section>
          <header className={styles.sectionHeader}>
            <h2>Known peers</h2>
            <span className={styles.count}>{knownPeers.length}</span>
          </header>
          <p className={styles.empty}>Known from routing and location telemetry. Live neighbors are marked below.</p>
          <ul className={styles.knownList}>
            {knownPeers.map((peer) => {
              const name = resolvePeerName(peer.addr, peerNames, peerLocations);
              const canShowOnMap = Boolean(peer.peerLocation?.position);
              return (
                <li key={peer.addr} className={styles.knownRow}>
                  <div className={styles.knownIdentity}>
                    <strong>{name || formatAddr(peer.addr)}</strong>
                    <span className={styles.knownAddr}>{formatAddr(peer.addr)}</span>
                  </div>
                  <div className={styles.knownSource}>
                    {peer.hasNeighbor ? (
                      <span className={styles.liveBadge}>Live neighbor</span>
                    ) : (
                      <span className={styles.knownBadge}>Known only</span>
                    )}
                    <span className={styles.knownMeta}>
                      {peer.hasRoute ? 'route' : ''}
                      {peer.hasRoute && peer.peerLocation ? ' + ' : ''}
                      {peer.peerLocation ? 'location' : ''}
                    </span>
                  </div>
                  <div className={styles.knownActions}>
                    <button type="button" className={styles.rowBtn} onClick={() => openDM(peer.addr)}>DM</button>
                    {canShowOnMap && (
                      <button type="button" className={styles.rowBtn} onClick={() => showOnMap(peer.addr)}>Show on map</button>
                    )}
                  </div>
                </li>
              );
            })}
          </ul>
        </section>
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
