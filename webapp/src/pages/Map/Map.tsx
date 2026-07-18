import { useEffect, useRef } from 'react';
import L from 'leaflet';
import 'leaflet/dist/leaflet.css';
import { useStore } from '../../store/index';
import type { PeerLocation, Route } from '../../types/bramble';
import { IconRoutes } from '../../components/Icons';
import { formatAddr0x } from '../../utils/address';
import styles from './Map.module.css';

// Fix Leaflet default icon paths broken by bundlers
import markerIcon2x from 'leaflet/dist/images/marker-icon-2x.png';
import markerIcon from 'leaflet/dist/images/marker-icon.png';
import markerShadow from 'leaflet/dist/images/marker-shadow.png';

delete (L.Icon.Default.prototype as any)._getIconUrl;
L.Icon.Default.mergeOptions({
  iconRetinaUrl: markerIcon2x,
  iconUrl: markerIcon,
  shadowUrl: markerShadow,
});

/** Convert coarse grid square (e.g. "AB12cd") to approximate center lat/lon */
export function gridSquareToLatLon(grid: string): [number, number] | null {
  if (!grid || grid.length < 4) return null;
  const A = grid.charCodeAt(0) - 65;
  const B = grid.charCodeAt(1) - 65;
  const n1 = parseInt(grid[2], 10);
  const n2 = parseInt(grid[3], 10);
  let lon = A * 20 - 180 + n1 * 2 + 1;
  let lat = B * 10 - 90 + n2 * 1 + 0.5;
  if (grid.length >= 6) {
    const a = grid.charCodeAt(4) - 97;
    const b = grid.charCodeAt(5) - 97;
    lon = A * 20 - 180 + n1 * 2 + a * (2 / 24) + (1 / 24);
    lat = B * 10 - 90 + n2 * 1 + b * (1 / 24) + (0.5 / 24);
  }
  return [lat, lon];
}

/** ~1km grid rectangle bounds for a 6-char grid square */
export function gridSquareBounds(grid: string): L.LatLngBoundsExpression | null {
  if (!grid || grid.length < 6) return null;
  const A = grid.charCodeAt(0) - 65;
  const B = grid.charCodeAt(1) - 65;
  const n1 = parseInt(grid[2], 10);
  const n2 = parseInt(grid[3], 10);
  const a = grid.charCodeAt(4) - 97;
  const b = grid.charCodeAt(5) - 97;
  const lonStep = 2 / 24;
  const latStep = 1 / 24;
  const lonBase = A * 20 - 180 + n1 * 2 + a * lonStep;
  const latBase = B * 10 - 90 + n2 * 1 + b * latStep;
  return [
    [latBase, lonBase],
    [latBase + latStep, lonBase + lonStep],
  ];
}

function nodeLabel(addr: number, name?: string): string {
  const hex = formatAddr0x(addr);
  return name ? `${name} (${hex})` : hex;
}

/** Resolve a node address to its map LatLng (if we know its position) */
function addrToLatLng(
  addr: number,
  peerLocations: PeerLocation[],
  selfPos: { lat: number; lon: number } | null | undefined,
  selfAddr: number | undefined
): L.LatLng | null {
  // Self?
  if (selfAddr !== undefined && addr === selfAddr && selfPos) {
    return L.latLng(selfPos.lat, selfPos.lon);
  }
  const peer = peerLocations.find(p => p.addr === addr);
  if (!peer) return null;
  if (peer.tier === 'full' && peer.position) {
    return L.latLng(peer.position.lat, peer.position.lon);
  }
  if (peer.tier === 'coarse' && peer.gridSquare) {
    const c = gridSquareToLatLon(peer.gridSquare);
    if (c) return L.latLng(c[0], c[1]);
  }
  return null;
}

/** State labels for route lines */
const STATE_COLORS: Record<string, string> = {
  active: '#3fb950',
  stale: '#d29922',
  broken: '#d73a49',
  discovering: '#8b949e',
};

export function Map() {
  const mapRef = useRef<HTMLDivElement>(null);
  const leafletMap = useRef<L.Map | null>(null);
  const markerLayer = useRef<L.LayerGroup | null>(null);
  const routeLayer = useRef<L.LayerGroup | null>(null);
  const lastFitPeerKeyRef = useRef<string | null>(null);

  const config = useStore(s => s.config);
  const connected = useStore(s => s.connectionState === 'connected');
  const peerLocations = useStore(s => s.peerLocations);
  const status = useStore(s => s.status);
  const peerNames = useStore(s => s.peerNames);
  const routes = useStore(s => s.routes);
  const showRoutes = useStore(s => s.showRoutes);
  const setShowRoutes = useStore(s => s.setShowRoutes);
  const mapFocusAddr = useStore(s => s.mapFocusAddr);
  const setMapFocusAddr = useStore(s => s.setMapFocusAddr);

  const selfAddr = config?.identity?.address;
  const configuredSelfName = config?.identity?.name?.trim();
  const selfName = configuredSelfName && configuredSelfName !== '(unnamed)'
    ? configuredSelfName
    : (selfAddr !== undefined ? peerNames.get(selfAddr) : undefined);
  const selfPeerLocation = selfAddr === undefined
    ? undefined
    : peerLocations.find(p => p.addr === selfAddr && p.tier === 'full' && !!p.position);
  const selfPos = status?.position ?? selfPeerLocation?.position ?? null;
  const gpsEnabled = config?.location?.enabled ?? false;
  const locationPolicyPreview = !config?.location
    ? 'Location policy unavailable'
    : !config.location.enabled
      ? 'Location sharing is OFF'
      : `Sharing ${config.location.default_tier} updates every ${config.location.interval_s}s via ${config.location.source}`;

  // Initialize map
  useEffect(() => {
    if (!mapRef.current || leafletMap.current) return;

    const initialCenter: L.LatLngExpression = selfPos
      ? [selfPos.lat, selfPos.lon]
      : [37.7749, -122.4194];
    const initialZoom = selfPos ? 13 : 2;

    const map = L.map(mapRef.current).setView(initialCenter, initialZoom);
    L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png', {
      attribution: '&copy; <a href="https://www.openstreetmap.org/copyright">OpenStreetMap</a>',
      maxZoom: 19,
    }).addTo(map);
    markerLayer.current = L.layerGroup().addTo(map);
    routeLayer.current = L.layerGroup().addTo(map);
    leafletMap.current = map;
    return () => { map.remove(); leafletMap.current = null; };
  }, []);

  // Update markers
  useEffect(() => {
    const map = leafletMap.current;
    const ml = markerLayer.current;
    if (!map || !ml) return;

    ml.clearLayers();
    const bounds: L.LatLng[] = [];
    const visiblePeerKey = peerLocations
      .filter(peer => (selfAddr === undefined || peer.addr !== selfAddr) && (
        (peer.tier === 'full' && !!peer.position) ||
        ((peer.tier === 'coarse' || peer.tier === 'presence') && !!peer.gridSquare)
      ))
      .map(peer => peer.addr)
      .sort((a, b) => a - b)
      .join(',');

    // Self
    if (selfPos) {
      const ll = L.latLng(selfPos.lat, selfPos.lon);
      bounds.push(ll);
      L.circleMarker(ll, {
        radius: 8, color: '#4a90d9', fillColor: '#4a90d9', fillOpacity: 0.8, weight: 2,
      }).bindPopup(
        `<b>${selfAddr !== undefined ? nodeLabel(selfAddr, selfName) : 'You'}</b><br/>` +
        `Accuracy: ${selfPos.accuracy.toFixed(0)}m`
      ).bindTooltip(
        selfAddr !== undefined ? nodeLabel(selfAddr, selfName) : 'You',
        { permanent: true, direction: 'top', offset: [0, -10], className: styles.nodeLabelTooltip }
      ).addTo(ml);
      if (selfPos.accuracy > 0) {
        L.circle(ll, {
          radius: selfPos.accuracy, color: '#4a90d9', fillColor: '#4a90d9', fillOpacity: 0.1, weight: 1,
        }).addTo(ml);
      }
    }

    // Peers
    for (const peer of peerLocations) {
      if (selfAddr !== undefined && peer.addr === selfAddr) continue;
      const peerDisplayName = peer.name || peerNames.get(peer.addr);

      if (peer.tier === 'full' && peer.position) {
        const ll = L.latLng(peer.position.lat, peer.position.lon);
        bounds.push(ll);
        L.circleMarker(ll, {
          radius: 7, color: '#4caf50', fillColor: '#4caf50', fillOpacity: 0.8, weight: 2,
        }).bindPopup(
          `<b>${nodeLabel(peer.addr, peerDisplayName)}</b><br/>Tier: exact<br/>Accuracy: ${peer.position.accuracy.toFixed(0)}m`
        ).bindTooltip(
          nodeLabel(peer.addr, peerDisplayName),
          { permanent: true, direction: 'top', offset: [0, -10], className: styles.nodeLabelTooltip }
        ).addTo(ml);
      } else if (peer.tier === 'coarse' && peer.gridSquare) {
        const rectBounds = gridSquareBounds(peer.gridSquare);
        if (rectBounds) {
          L.rectangle(rectBounds, {
            color: '#ffc107', fillColor: '#ffc107', fillOpacity: 0.25, weight: 2,
          }).bindPopup(
            `<b>${nodeLabel(peer.addr, peerDisplayName)}</b><br/>Tier: zone<br/>Grid: ${peer.gridSquare}`
          ).bindTooltip(
            nodeLabel(peer.addr, peerDisplayName),
            { permanent: true, direction: 'center', className: styles.nodeLabelTooltip }
          ).addTo(ml);
          const center = gridSquareToLatLon(peer.gridSquare);
          if (center) bounds.push(L.latLng(center[0], center[1]));
        }
      } else if (peer.tier === 'presence' && peer.gridSquare) {
        const center = gridSquareToLatLon(peer.gridSquare);
        if (center) {
          const ll = L.latLng(center[0], center[1]);
          bounds.push(ll);
          L.circleMarker(ll, {
            radius: 6,
            color: '#9aa4b2',
            fillColor: '#9aa4b2',
            fillOpacity: 0.35,
            weight: 2,
            dashArray: '3 3',
          }).bindPopup(
            `<b>${nodeLabel(peer.addr, peerDisplayName)}</b><br/>Tier: presence<br/>Approximate location (grid: ${peer.gridSquare})`
          ).bindTooltip(
            `${nodeLabel(peer.addr, peerDisplayName)} · approximate location`,
            { permanent: true, direction: 'top', offset: [0, -10], className: styles.nodeLabelTooltip }
          ).addTo(ml);
        }
      }
    }

    if (bounds.length > 0 && lastFitPeerKeyRef.current !== visiblePeerKey) {
      map.fitBounds(L.latLngBounds(bounds).pad(0.2));
      lastFitPeerKeyRef.current = visiblePeerKey;
    }
  }, [selfPos, selfAddr, selfName, peerLocations, peerNames]);

  // Draw route lines
  useEffect(() => {
    const map = leafletMap.current;
    const rl = routeLayer.current;
    if (!map || !rl) return;
    rl.clearLayers();

    if (!showRoutes || !selfPos) return;

    for (const route of routes) {
      const destLL = addrToLatLng(route.dest, peerLocations, selfPos, selfAddr);
      if (!destLL) continue;

      const selfLL = L.latLng(selfPos.lat, selfPos.lon);
      const color = STATE_COLORS[route.state] ?? '#8b949e';
      const isDirect = route.hopCount <= 1;

      const line = L.polyline([selfLL, destLL], {
        color,
        weight: isDirect ? 2 : 1.5,
        opacity: route.state === 'broken' ? 0.4 : 0.7,
        dashArray: isDirect ? undefined : '6 4',
      }).addTo(rl);

      // Tooltip with route details
      const hopLabel = route.hopCount === 1 ? '1 hop (direct)' : `${route.hopCount} hops`;
      const stateLabel = route.state.charAt(0).toUpperCase() + route.state.slice(1);
      const destName = route.dest === selfAddr ? selfName : peerNames.get(route.dest);
      const nextHopName = route.nextHop === selfAddr ? selfName : peerNames.get(route.nextHop);
      line.bindTooltip(
        `<b>${nodeLabel(route.dest, destName)}</b><br/>` +
        `${hopLabel} · via ${nodeLabel(route.nextHop, nextHopName)}<br/>` +
        `Metric: ${route.metric} · ${stateLabel}`,
        { sticky: true, direction: 'top', className: styles.routeTooltip }
      );
    }
  }, [routes, peerLocations, selfPos, selfAddr, selfName, peerNames, showRoutes]);

  // Handle focus request from Nodes page
  useEffect(() => {
    if (mapFocusAddr === null) return;
    const map = leafletMap.current;
    if (!map) return;

    const ll = addrToLatLng(mapFocusAddr, peerLocations, selfPos, selfAddr);
    if (ll) {
      map.setView(ll, 15, { animate: true });
      // Flash a temporary highlight circle
      const highlight = L.circleMarker(ll, {
        radius: 20, color: '#ffc107', fillColor: '#ffc107', fillOpacity: 0.3, weight: 2,
      }).addTo(map);
      setTimeout(() => map.removeLayer(highlight), 2000);
    }

    // Clear focus so clicking badge again re-triggers
    setMapFocusAddr(null);
  }, [mapFocusAddr, peerLocations, selfPos, selfAddr]);

  const hasPeers = peerLocations.length > 0;

  // Config hasn't arrived since connect yet: distinguish "still loading" from
  // "loaded, nothing to show" so a slow ESP32 link doesn't look like GPS is
  // simply off.
  if (connected && config === null) {
    return (
      <div className={styles.wrapper}>
        <div className={styles.empty}>Loading map…</div>
      </div>
    );
  }

  if (!gpsEnabled && !hasPeers) {
    return (
      <div className={styles.wrapper}>
        <div className={styles.empty}>
          GPS is disabled and no peer locations available.<br />
          Enable location sharing in Config to see the map.
        </div>
      </div>
    );
  }

  return (
    <div className={styles.wrapper}>
      <div ref={mapRef} className={styles.mapContainer} />
      <div className={styles.legend}>
        <span className={styles.legendItem}>{locationPolicyPreview}</span>
        <span className={styles.legendItem}>
          <span className={`${styles.dot} ${styles.dotBlue}`} /> You
        </span>
        <span className={styles.legendItem}>
          <span className={`${styles.dot} ${styles.dotGreen}`} /> Exact peer
        </span>
        <span className={styles.legendItem}>
          <span className={`${styles.dot} ${styles.dotYellow}`} /> Zone peer
        </span>
        <span className={styles.legendItem}>
          <span className={`${styles.dot} ${styles.dotGray}`} /> Presence peer (approx.)
        </span>

        <span className={styles.legendItem}>
          <span className={styles.routeLineDirect} /> Direct (1 hop)
        </span>
        <span className={styles.legendItem}>
          <span className={styles.routeLineMultiHop} /> Multi-hop
        </span>
        <span className={styles.legendItem}>
          <span className={styles.routeStateActive}>■</span> Active
        </span>
        <span className={styles.legendItem}>
          <span className={styles.routeStateStale}>■</span> Stale
        </span>
        <span className={styles.legendItem}>
          <span className={styles.routeStateBroken}>■</span> Broken
        </span>
        <span className={styles.legendItem}>
          <span className={styles.routeStateDiscovering}>■</span> Discovering
        </span>

        <button
          className={`${styles.routeToggle} ${showRoutes ? styles.routeToggleActive : ''}`}
          onClick={() => setShowRoutes(!showRoutes)}
          title="Show/hide route lines between you and known destinations"
        >
          <IconRoutes size={14} /> Routes
        </button>
      </div>
    </div>
  );
}
