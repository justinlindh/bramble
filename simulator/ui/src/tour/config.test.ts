import { describe, expect, it } from 'vitest';
import { tourOverrideFromSearch } from './config';

describe('tourOverrideFromSearch', () => {
  it('returns null when the page said nothing, so the server decides', () => {
    expect(tourOverrideFromSearch('')).toBeNull();
    expect(tourOverrideFromSearch('?scenario=emu-playground')).toBeNull();
  });

  it('turns the tour on for the forms a person would type', () => {
    expect(tourOverrideFromSearch('?tour=1')).toBe(true);
    expect(tourOverrideFromSearch('?tour=true')).toBe(true);
    expect(tourOverrideFromSearch('?tour')).toBe(true);
  });

  it('turns the tour off explicitly, overriding a playground server', () => {
    expect(tourOverrideFromSearch('?tour=0')).toBe(false);
    expect(tourOverrideFromSearch('?tour=false')).toBe(false);
    expect(tourOverrideFromSearch('?tour=off')).toBe(false);
  });
});
