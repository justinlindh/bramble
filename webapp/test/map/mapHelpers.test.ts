import { describe, it, expect } from 'vitest';
import { gridSquareToLatLon, gridSquareBounds } from '../../src/pages/Map/Map';

describe('Map helpers', () => {
  it('converts 4-char grid squares to approximate center coordinates', () => {
    const result = gridSquareToLatLon('CM87');
    expect(result).not.toBeNull();
    const [lat, lon] = result!;
    expect(lat).toBeGreaterThan(-90);
    expect(lat).toBeLessThan(90);
    expect(lon).toBeGreaterThan(-180);
    expect(lon).toBeLessThan(180);
  });

  it('returns null for invalid grid square values', () => {
    expect(gridSquareToLatLon('')).toBeNull();
    expect(gridSquareBounds('CM87')).toBeNull(); // needs 6 chars
  });

  it('returns rectangle bounds for 6-char grid square', () => {
    const bounds = gridSquareBounds('CM87ss');
    expect(bounds).not.toBeNull();
    const b = bounds as [[number, number], [number, number]];
    expect(b[1][0]).toBeGreaterThan(b[0][0]);
    expect(b[1][1]).toBeGreaterThan(b[0][1]);
  });
});
