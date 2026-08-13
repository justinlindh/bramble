import { describe, it, expect } from 'vitest';
import { gridSquareToLatLon, gridSquareBounds } from '../../src/pages/Map/Map';

describe('Map helpers', () => {
  it('converts 4-char grid squares to the center of the 2 x 1 degree square', () => {
    const result = gridSquareToLatLon('CM87');
    expect(result).not.toBeNull();
    const [lat, lon] = result!;
    // CM87 spans lon [-124, -122) and lat [37, 38), so its center is one
    // degree east of the corner and half a degree north of it.
    expect(lat).toBeCloseTo(37.5, 10);
    expect(lon).toBeCloseTo(-123, 10);
  });

  it('converts 6-char grid squares to the center of the subsquare', () => {
    const result = gridSquareToLatLon('CM87ss');
    expect(result).not.toBeNull();
    const [lat, lon] = result!;
    // Subsquare 's' is the 19th of 24, so the corner sits 18 steps into the
    // CM87 square (lat 37.75, lon -122.5) and the center adds half a step.
    expect(lat).toBeCloseTo(37.770833333333336, 10);
    expect(lon).toBeCloseTo(-122.45833333333333, 10);
  });

  it('returns null for invalid grid square values', () => {
    expect(gridSquareToLatLon('')).toBeNull();
    expect(gridSquareBounds('CM87')).toBeNull(); // needs 6 chars
  });

  it('returns the subsquare corner and its opposite corner as bounds', () => {
    const bounds = gridSquareBounds('CM87ss');
    expect(bounds).not.toBeNull();
    const b = bounds as [[number, number], [number, number]];
    expect(b[0][0]).toBeCloseTo(37.75, 10);
    expect(b[0][1]).toBeCloseTo(-122.5, 10);
    expect(b[1][0]).toBeCloseTo(37.791666666666664, 10);
    expect(b[1][1]).toBeCloseTo(-122.41666666666667, 10);
  });

  it('places the marker at the center of the rectangle for the same locator', () => {
    // Both helpers decode through one shared cell, so a decode bug would move
    // the marker and the rectangle together. Pin the relationship between them.
    const [lat, lon] = gridSquareToLatLon('CM87ss')!;
    const b = gridSquareBounds('CM87ss') as [[number, number], [number, number]];
    expect(lat).toBeCloseTo((b[0][0] + b[1][0]) / 2, 10);
    expect(lon).toBeCloseTo((b[0][1] + b[1][1]) / 2, 10);
  });
});
