import { useEffect, useRef } from 'react';
import L from 'leaflet';
import 'leaflet/dist/leaflet.css';
import { useStore } from '../../store/index';
import type { PeerLocation } from '../../types/bramble';
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
function gridSquareToLatLon(grid: string): [number, number] | null {
  if (!grid || grid.length < 4) return null;
  const A = grid.charCodeAt(0) - 65; // 0-17
  const B = grid.charCodeAt(1) - 65;
  const n1 = parseInt(grid[2], 10);
  const n2 = parseInt(grid[3], 10);
  let lon = A * 20 - 180 + n1 * 2 + 1; // center of 2° cell
  let lat = B * 10 - 90 + n2 * 1 + 0.5; // center of 1° cell
  if (grid.length >= 6) {
    const a = grid.charCodeAt(4) - 97; // 0-23
    const b = grid.charCodeAt(5) - 97;
    lon = A * 20 - 180 + n1 * 2 + a * (2 / 24) + (1 / 24);
    lat = B * 10 - 90 + n2 * 1 + b * (1 / 24) + (0.5 / 24);
  }
  return [lat, lon];
}

/** ~1km grid rectangle bounds for a 6-char grid square */
function gridSquareBounds(grid: string): L.LatLngBoundsExpression | null {
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

export function Map() {
  const mapRef = useRef<HTMLDivElement>(null);
  const leafletMap = useRef<L.Map | null>(null);
  const layerGroup = useRef<L.LayerGroup | null>(null);

  const config = useStore(s => s.config);
  const peerLocations = useStore(s => s.peerLocations);
  const status = useStore(s => s.status);

  // Self position from status
  const selfPos = status?.position ?? null;
  const gpsEnabled = config?.location?.enabled ?? false;

  // Initialize map
  useEffect(() => {
    if (!mapRef.current || leafletMap.current) return;
    const map = L.map(mapRef.current).setView([37.7749, -122.4194], 10);
    L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png', {
      attribution: '&copy; <a href="https://www.openstreetmap.org/copyright">OpenStreetMap</a>',
      maxZoom: 19,
    }).addTo(map);
    layerGroup.current = L.layerGroup().addTo(map);
    leafletMap.current = map;

    return () => {
      map.remove();
      leafletMap.current = null;
    };
  }, []);

  // Update markers when data changes
  useEffect(() => {
    const map = leafletMap.current;
    const lg = layerGroup.current;
    if (!map || !lg) return;

    lg.clearLayers();
    const bounds: L.LatLng[] = [];

    // Self position — blue circle
    if (selfPos) {
      const ll = L.latLng(selfPos.lat, selfPos.lon);
      bounds.push(ll);
      L.circleMarker(ll, {
        radius: 8,
        color: '#4a90d9',
        fillColor: '#4a90d9',
        fillOpacity: 0.8,
        weight: 2,
      }).bindPopup(`<b>You</b><br/>Accuracy: ${selfPos.accuracy.toFixed(0)}m`).addTo(lg);

      // Accuracy circle
      if (selfPos.accuracy > 0) {
        L.circle(ll, {
          radius: selfPos.accuracy,
          color: '#4a90d9',
          fillColor: '#4a90d9',
          fillOpacity: 0.1,
          weight: 1,
        }).addTo(lg);
      }
    }

    // Peer locations
    for (const peer of peerLocations) {
      if (peer.tier === 'full' && peer.position) {
        // Exact — green circle
        const ll = L.latLng(peer.position.lat, peer.position.lon);
        bounds.push(ll);
        L.circleMarker(ll, {
          radius: 7,
          color: '#4caf50',
          fillColor: '#4caf50',
          fillOpacity: 0.8,
          weight: 2,
        }).bindPopup(
          `<b>${peer.name || `0x${peer.addr.toString(16).toUpperCase()}`}</b><br/>` +
          `Tier: exact<br/>` +
          `Accuracy: ${peer.position.accuracy.toFixed(0)}m`
        ).addTo(lg);
      } else if (peer.tier === 'coarse' && peer.gridSquare) {
        // Zone — yellow rectangle
        const rectBounds = gridSquareBounds(peer.gridSquare);
        if (rectBounds) {
          const rect = L.rectangle(rectBounds, {
            color: '#ffc107',
            fillColor: '#ffc107',
            fillOpacity: 0.25,
            weight: 2,
          }).bindPopup(
            `<b>${peer.name || `0x${peer.addr.toString(16).toUpperCase()}`}</b><br/>` +
            `Tier: zone<br/>` +
            `Grid: ${peer.gridSquare}`
          ).addTo(lg);
          // Add center to bounds
          const center = gridSquareToLatLon(peer.gridSquare);
          if (center) bounds.push(L.latLng(center[0], center[1]));
        }
      }
    }

    // Auto-fit bounds
    if (bounds.length > 0) {
      map.fitBounds(L.latLngBounds(bounds).pad(0.2));
    }
  }, [selfPos, peerLocations]);

  const hasPeers = peerLocations.length > 0;
  const hasAnything = selfPos || hasPeers;

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
        <span className={styles.legendItem}>
          <span className={`${styles.dot} ${styles.dotBlue}`} /> You
        </span>
        <span className={styles.legendItem}>
          <span className={`${styles.dot} ${styles.dotGreen}`} /> Exact peer
        </span>
        <span className={styles.legendItem}>
          <span className={`${styles.dot} ${styles.dotYellow}`} /> Zone peer
        </span>
      </div>
    </div>
  );
}
