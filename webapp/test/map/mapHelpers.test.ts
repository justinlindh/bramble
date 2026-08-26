import { describe, it, expect } from 'vitest';
import {
  coarseZoneBounds,
  coarseZoneCenter,
  COARSE_CELL_LAT_DEG,
  COARSE_CELL_LON_DEG,
} from '../../src/pages/Map/coarseZone';

/**
 * Mirror of location_serialize_coarse / location_deserialize_coarse in
 * components/location/location.c, so the zone the map draws can be checked
 * against what the firmware actually transmits rather than against a second
 * reading of the same assumption.
 */
function quantizeCoarse(deg: number, groupUnits: number, offset: number): number {
  const e7 = Math.round(deg * 1e7);
  const units = Math.trunc(e7 / 10000); // C integer division truncates toward zero
  const enc = Math.floor((units + offset) / groupUnits);
  return ((enc * groupUnits - offset) * 10000) / 1e7;
}

const quantLat = (lat: number) => quantizeCoarse(lat, 3, 90000);
const quantLon = (lon: number) => quantizeCoarse(lon, 6, 180000);

type Bounds = [[number, number], [number, number]];

describe('coarse zone geometry', () => {
  it('spans one quantization cell from the decoded corner', () => {
    const b = coarseZoneBounds(39.993, -105.042) as Bounds;
    expect(b[0][0]).toBeCloseTo(39.993, 9);
    expect(b[1][0]).toBeCloseTo(39.993 + COARSE_CELL_LAT_DEG, 9);
    expect(b[1][1]).toBeCloseTo(-105.042 + COARSE_CELL_LON_DEG, 9);
  });

  it('widens a negative corner by one unit, because C division truncates toward zero', () => {
    // A negative e7 divided by 10000 rounds toward the equator, so the true
    // position can sit one thousandth of a degree below the decoded corner.
    const b = coarseZoneBounds(-33.867, 18.003) as Bounds;
    expect(b[0][0]).toBeCloseTo(-33.868, 9);
    expect(b[0][1]).toBeCloseTo(18.003, 9);
  });

  it('puts the center inside the rectangle it belongs to', () => {
    const [lat, lon] = coarseZoneCenter(39.993, -105.042);
    const b = coarseZoneBounds(39.993, -105.042) as Bounds;
    expect(lat).toBeGreaterThan(b[0][0]);
    expect(lat).toBeLessThan(b[1][0]);
    expect(lon).toBeGreaterThan(b[0][1]);
    expect(lon).toBeLessThan(b[1][1]);
  });

  /*
   * The property that matters for a privacy control: whatever the peer's real
   * position was, the zone drawn from the quantized share must contain it. A
   * zone that excludes the peer would point somewhere they are not.
   */
  it('contains every true position that quantizes into it', () => {
    const samples: Array<[number, number]> = [];
    for (let lat = -89; lat <= 89; lat += 7.3) {
      for (let lon = -179; lon <= 179; lon += 13.7) {
        samples.push([lat + 0.0004, lon + 0.0011]);
        samples.push([lat + 0.0029, lon + 0.0058]);
        samples.push([lat, lon]);
      }
    }
    // Straddle the sign boundaries, where the truncation asymmetry lives.
    for (const edge of [-0.0025, -0.001, -0.0001, 0, 0.0001, 0.001, 0.0025]) {
      samples.push([edge, edge]);
    }

    let checked = 0;
    for (const [trueLat, trueLon] of samples) {
      const b = coarseZoneBounds(quantLat(trueLat), quantLon(trueLon)) as Bounds;
      expect(trueLat).toBeGreaterThanOrEqual(b[0][0] - 1e-9);
      expect(trueLat).toBeLessThanOrEqual(b[1][0] + 1e-9);
      expect(trueLon).toBeGreaterThanOrEqual(b[0][1] - 1e-9);
      expect(trueLon).toBeLessThanOrEqual(b[1][1] + 1e-9);
      checked++;
    }
    expect(checked).toBeGreaterThan(1000);
  });

  it('never draws a zone wider than two quantization cells', () => {
    // The containment property above is trivially satisfiable by drawing an
    // enormous rectangle, so bound the other side too.
    for (const [lat, lon] of [[39.993, -105.042], [-33.867, 18.003], [0, 0]]) {
      const b = coarseZoneBounds(lat, lon) as Bounds;
      expect(b[1][0] - b[0][0]).toBeLessThanOrEqual(2 * COARSE_CELL_LAT_DEG);
      expect(b[1][1] - b[0][1]).toBeLessThanOrEqual(2 * COARSE_CELL_LON_DEG);
    }
  });
});
