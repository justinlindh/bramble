import type { LatLngBoundsExpression } from 'leaflet';

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

/**
 * The four edges of the cell a coarse share decodes to. The drawn rectangle
 * and its center both derive from this one computation so they cannot describe
 * different rectangles: on a privacy control the marker must sit inside the
 * zone the peer is shown, and a drift between the two would put it elsewhere.
 */
function coarseZoneCorners(lat: number, lon: number): {
  latLow: number;
  lonLow: number;
  latHigh: number;
  lonHigh: number;
} {
  return {
    latLow: coarseCellLow(lat),
    lonLow: coarseCellLow(lon),
    latHigh: lat + COARSE_CELL_LAT_DEG,
    lonHigh: lon + COARSE_CELL_LON_DEG,
  };
}

/** Rectangle covering every position that quantizes to this coarse share. */
export function coarseZoneBounds(lat: number, lon: number): LatLngBoundsExpression {
  const { latLow, lonLow, latHigh, lonHigh } = coarseZoneCorners(lat, lon);
  return [
    [latLow, lonLow],
    [latHigh, lonHigh],
  ];
}

/** Center of the coarse zone, for fitting bounds and drawing a marker. */
export function coarseZoneCenter(lat: number, lon: number): [number, number] {
  const { latLow, lonLow, latHigh, lonHigh } = coarseZoneCorners(lat, lon);
  return [(latLow + latHigh) / 2, (lonLow + lonHigh) / 2];
}
