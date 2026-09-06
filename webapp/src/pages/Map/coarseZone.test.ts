import { describe, it, expect } from 'vitest';
import {
  COARSE_CELL_LAT_DEG,
  COARSE_CELL_LON_DEG,
  coarseZoneBounds,
  coarseZoneCenter,
} from './coarseZone';

const UNIT = 0.001;

// Pull the four numeric edges out of the [[latLow, lonLow], [latHigh, lonHigh]]
// bounds shape so the assertions can read them by name.
function edges(lat: number, lon: number) {
  const bounds = coarseZoneBounds(lat, lon) as [[number, number], [number, number]];
  const [[latLow, lonLow], [latHigh, lonHigh]] = bounds;
  return { latLow, lonLow, latHigh, lonHigh };
}

describe('coarseZone geometry', () => {
  it('uses cell sizes of three units of latitude and six of longitude', () => {
    expect(COARSE_CELL_LAT_DEG).toBeCloseTo(3 * UNIT, 12);
    expect(COARSE_CELL_LON_DEG).toBeCloseTo(6 * UNIT, 12);
  });

  it('anchors a positive decoded corner at the low edge of the cell', () => {
    const { latLow, lonLow, latHigh, lonHigh } = edges(1.5, 2.5);
    expect(latLow).toBeCloseTo(1.5, 12);
    expect(lonLow).toBeCloseTo(2.5, 12);
    expect(latHigh).toBeCloseTo(1.503, 12);
    expect(lonHigh).toBeCloseTo(2.506, 12);
  });

  it('widens a negative cell by one unit below the corner', () => {
    // The firmware truncates e7 division toward zero, so a cell at or below
    // zero also holds true positions one unit further from zero than its
    // decoded corner. The drawn zone must extend to cover them.
    const { latLow, lonLow, latHigh, lonHigh } = edges(-1.5, -2.5);
    expect(latLow).toBeCloseTo(-1.501, 12);
    expect(lonLow).toBeCloseTo(-2.501, 12);
    expect(latHigh).toBeCloseTo(-1.497, 12);
    expect(lonHigh).toBeCloseTo(-2.494, 12);
    // Widened cell spans four latitude units and seven longitude units.
    expect(latHigh - latLow).toBeCloseTo(4 * UNIT, 12);
    expect(lonHigh - lonLow).toBeCloseTo(7 * UNIT, 12);
  });

  it('treats a zero corner as non-positive and widens below it', () => {
    const { latLow, lonLow, latHigh, lonHigh } = edges(0, 0);
    expect(latLow).toBeCloseTo(-UNIT, 12);
    expect(lonLow).toBeCloseTo(-UNIT, 12);
    expect(latHigh).toBeCloseTo(3 * UNIT, 12);
    expect(lonHigh).toBeCloseTo(6 * UNIT, 12);
  });

  it('keeps the center strictly inside the drawn bounds', () => {
    // The privacy contract is that the marker sits inside the zone the peer is
    // shown; a center outside the rectangle would place it elsewhere.
    for (const [lat, lon] of [
      [1.5, 2.5],
      [-1.5, -2.5],
      [0, 0],
      [12.3456, -98.7654],
    ] as const) {
      const { latLow, lonLow, latHigh, lonHigh } = edges(lat, lon);
      const [centerLat, centerLon] = coarseZoneCenter(lat, lon);
      expect(centerLat).toBeGreaterThan(latLow);
      expect(centerLat).toBeLessThan(latHigh);
      expect(centerLon).toBeGreaterThan(lonLow);
      expect(centerLon).toBeLessThan(lonHigh);
    }
  });

  it('derives center and bounds from the same corners', () => {
    const { latLow, lonLow, latHigh, lonHigh } = edges(12.3456, -98.7654);
    const [centerLat, centerLon] = coarseZoneCenter(12.3456, -98.7654);
    expect(centerLat).toBeCloseTo((latLow + latHigh) / 2, 12);
    expect(centerLon).toBeCloseTo((lonLow + lonHigh) / 2, 12);
  });
});
