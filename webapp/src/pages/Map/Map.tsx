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

/*
 * Coarse ("zone") tier geometry.
 *
 * A coarse share carries a quantized position, not a locator string. The
 * firmware (location_serialize_coarse in components/location/location.c)
 * divides latitude_e7 and longitude_e7 by 10000, giving units of one
 * thousandth of a degree, then groups those units in threes for latitude and
 * sixes for longitude. What arrives is the decoded corner of that cell, so
 * the peer is somewhere inside it and nowhere more precise.
 */
const COARSE_UNIT_DEG = 0.001;
export const COARSE_CELL_LAT_DEG = 3 * COARSE_UNIT_DEG;
export const COARSE_CELL_LON_DEG = 6 * COARSE_UNIT_DEG;

/**
 * Lower edge of the cell whose decoded corner is `deg`. The firmware divides
 * a signed e7 value with C semantics, which truncate toward zero rather than
 * flooring, so a cell at or below zero also holds true positions one unit
 * further from zero than its decoded corner. Widening by that unit keeps the
 * drawn zone a superset of where the peer can be, which is the only safe
 * direction to be wrong on a privacy control.
 */
function coarseCellLow(deg: number): number {
  return deg > 0 ? deg : deg - COARSE_UNIT_DEG;
}

/** Rectangle covering every position that quantizes to this coarse share. */
export function coarseZoneBounds(lat: number, lon: number): L.LatLngBoundsExpression {
  const latLow = coarseCellLow(lat);
  const lonLow = coarseCellLow(lon);
  return [
    [latLow, lonLow],
    [lat + COARSE_CELL_LAT_DEG, lon + COARSE_CELL_LON_DEG],
  ];
}

/** Center of the coarse zone, for fitting bounds and drawing a marker. */
export function coarseZoneCenter(lat: number, lon: number): [number, number] {
  const latLow = coarseCellLow(lat);
  const lonLow = coarseCellLow(lon);
  return [
    (latLow + lat + COARSE_CELL_LAT_DEG) / 2,
    (lonLow + lon + COARSE_CELL_LON_DEG) / 2,
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
  if (peer.tier === 'coarse' && peer.position) {
    const c = coarseZoneCenter(peer.position.lat, peer.position.lon);
    return L.latLng(c[0], c[1]);
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
  // Sharing publishes only to configured targets, so the affirmative wording
  // is earned by having one, not by the policy switch being on.
  const shareTargetCount =
    (config?.location?.contact_rules ?? []).filter(r => r.enabled !== false).length +
    (config?.location?.channel_targets ?? []).filter(c => c.enabled !== false).length;
  const locationPolicyPreview = !config?.location
    ? 'Location policy unavailable'
    : !config.location.enabled
      ? 'Location sharing is OFF'
      : shareTargetCount === 0
        ? 'Location sharing is ON with no targets, so nothing is published'
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
      .filter(peer => (selfAddr === undefined || peer.addr !== selfAddr) &&
        (peer.tier === 'full' || peer.tier === 'coarse') && !!peer.position)
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
      } else if (peer.tier === 'coarse' && peer.position) {
        const { lat, lon } = peer.position;
        L.rectangle(coarseZoneBounds(lat, lon), {
          color: '#ffc107', fillColor: '#ffc107', fillOpacity: 0.25, weight: 2,
        }).bindPopup(
          `<b>${nodeLabel(peer.addr, peerDisplayName)}</b><br/>Tier: zone<br/>` +
          'Somewhere in this area, not at a point inside it.'
        ).bindTooltip(
          nodeLabel(peer.addr, peerDisplayName),
          { permanent: true, direction: 'center', className: styles.nodeLabelTooltip }
        ).addTo(ml);
        const center = coarseZoneCenter(lat, lon);
        bounds.push(L.latLng(center[0], center[1]));
      }
      /* A presence-tier peer shares no coordinates at all, so there is
       * nothing to place: the Nodes list is where that peer shows up. */
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
