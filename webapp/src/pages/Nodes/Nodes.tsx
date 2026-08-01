import { useEffect } from 'react';
import { useStore } from '../../store/index';
import { loadNeighbors, loadRoutes, loadPeerLocations, openDM, showOnMap } from '../../store/actions';
import { resolvePeerName } from '../../store/peerName';
import { usePoll } from '../../hooks/usePoll';
import { NeighborCard } from './NeighborCard';
import { RouteTable } from './RouteTable';
import { IconNodes, IconRoutes } from '../../components/Icons';
import { formatAddr0x } from '../../utils/address';
import styles from './Nodes.module.css';
import { buildKnownPeers } from './knownPeers';

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

  // Refresh policy. Neighbors are NOT polled here: bramble.onNeighborChange
  // pushes every add or refresh, and App keeps a single 60s safety net for the
  // silent firmware neighbor_purge. Routes have no push event at all (the
  // firmware never emits bramble.onRouteUpdate), so they keep a real poll,
  // slowed to 30s. Peer locations are pushed by bramble.onPeerLocation and
  // bramble.onGpsEvent, so 60s is only a backstop for cache expiry.
  // All polls are gated on connection state so a disconnected tab is silent
  // rather than relying on the `if (!client) return` guard in actions.
  usePoll(loadRoutes, 30_000, { enabled: connected });
  usePoll(loadPeerLocations, 60_000, { enabled: connected });

  // One-shot neighbor refresh on entering the tab, so opening Nodes never
  // shows data up to a full global-poll interval old. Not a poll.
  useEffect(() => {
    if (connected) loadNeighbors().catch(() => {});
  }, [connected]);

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
                    <strong>{name || formatAddr0x(peer.addr)}</strong>
                    <span className={styles.knownAddr}>{formatAddr0x(peer.addr)}</span>
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
