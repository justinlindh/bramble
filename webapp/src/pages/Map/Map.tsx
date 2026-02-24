import { useEffect, useRef } from 'react';
import L from 'leaflet';
import 'leaflet/dist/leaflet.css';
import { useStore } from '../../store/index';
import type { PeerLocation, Route } from '../../types/bramble';
import { IconRoutes } from '../../components/Icons';
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

/** Format address as hex */
export function fmtAddr(addr: number): string {
  return `0x${addr.toString(16).toUpperCase()}`;
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

  const config = useStore(s => s.config);
  const peerLocations = useStore(s => s.peerLocations);
  const status = useStore(s => s.status);
  const routes = useStore(s => s.routes);
  const showRoutes = useStore(s => s.showRoutes);
  const setShowRoutes = useStore(s => s.setShowRoutes);
  const mapFocusAddr = useStore(s => s.mapFocusAddr);
  const setMapFocusAddr = useStore(s => s.setMapFocusAddr);

  const selfPos = status?.position ?? null;
  const selfAddr = config?.identity?.address;
  const gpsEnabled = config?.location?.enabled ?? false;
  const locationPolicyPreview = !config?.location
    ? 'Location policy unavailable'
    : !config.location.enabled
      ? 'Location sharing is OFF'
      : `Sharing ${config.location.default_tier} updates every ${config.location.interval_s}s via ${config.location.source}`;

  // Initialize map
  useEffect(() => {
    if (!mapRef.current || leafletMap.current) return;
    const map = L.map(mapRef.current).setView([37.7749, -122.4194], 10);
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

    // Self
    if (selfPos) {
      const ll = L.latLng(selfPos.lat, selfPos.lon);
      bounds.push(ll);
      L.circleMarker(ll, {
        radius: 8, color: '#4a90d9', fillColor: '#4a90d9', fillOpacity: 0.8, weight: 2,
      }).bindPopup(`<b>You</b><br/>Accuracy: ${selfPos.accuracy.toFixed(0)}m`).addTo(ml);
      if (selfPos.accuracy > 0) {
        L.circle(ll, {
          radius: selfPos.accuracy, color: '#4a90d9', fillColor: '#4a90d9', fillOpacity: 0.1, weight: 1,
        }).addTo(ml);
      }
    }

    // Peers
    for (const peer of peerLocations) {
      if (peer.tier === 'full' && peer.position) {
        const ll = L.latLng(peer.position.lat, peer.position.lon);
        bounds.push(ll);
        L.circleMarker(ll, {
          radius: 7, color: '#4caf50', fillColor: '#4caf50', fillOpacity: 0.8, weight: 2,
        }).bindPopup(
          `<b>${peer.name || fmtAddr(peer.addr)}</b><br/>Tier: exact<br/>Accuracy: ${peer.position.accuracy.toFixed(0)}m`
        ).addTo(ml);
      } else if (peer.tier === 'coarse' && peer.gridSquare) {
        const rectBounds = gridSquareBounds(peer.gridSquare);
        if (rectBounds) {
          L.rectangle(rectBounds, {
            color: '#ffc107', fillColor: '#ffc107', fillOpacity: 0.25, weight: 2,
          }).bindPopup(
            `<b>${peer.name || fmtAddr(peer.addr)}</b><br/>Tier: zone<br/>Grid: ${peer.gridSquare}`
          ).addTo(ml);
          const center = gridSquareToLatLon(peer.gridSquare);
          if (center) bounds.push(L.latLng(center[0], center[1]));
        }
      }
    }

    if (bounds.length > 0) {
      map.fitBounds(L.latLngBounds(bounds).pad(0.2));
    }
  }, [selfPos, peerLocations]);

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
      line.bindTooltip(
        `<b>${fmtAddr(route.dest)}</b><br/>` +
        `${hopLabel} · via ${fmtAddr(route.nextHop)}<br/>` +
        `Metric: ${route.metric} · ${stateLabel}`,
        { sticky: true, direction: 'top', className: styles.routeTooltip }
      );
    }
  }, [routes, peerLocations, selfPos, selfAddr, showRoutes]);

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
