import { describe, it, expect, vi, beforeEach } from 'vitest';
import { render, screen } from '@testing-library/react';

const mapMock = {
  setView: vi.fn(function () { return mapMock; }),
  fitBounds: vi.fn(),
  remove: vi.fn(),
  removeLayer: vi.fn(),
};

vi.mock('leaflet', () => {
  const L = {
    Icon: { Default: { prototype: {}, mergeOptions: vi.fn() } },
    map: vi.fn(() => mapMock),
    tileLayer: vi.fn(() => ({ addTo: vi.fn() })),
    layerGroup: vi.fn(() => {
      const group: any = { addTo: vi.fn(() => group), clearLayers: vi.fn() };
      return group;
    }),
    latLng: vi.fn((lat: number, lon: number) => ({ lat, lng: lon })),
    latLngBounds: vi.fn(() => ({ pad: vi.fn(() => ({ mockedBounds: true })) })),
    circleMarker: vi.fn(() => ({ bindPopup: vi.fn(() => ({})), bindTooltip: vi.fn(() => ({})), addTo: vi.fn(() => ({})) })),
    circle: vi.fn(() => ({ addTo: vi.fn() })),
    rectangle: vi.fn(() => ({ bindPopup: vi.fn(() => ({})), bindTooltip: vi.fn(() => ({})), addTo: vi.fn(() => ({})) })),
    polyline: vi.fn(() => ({ bindTooltip: vi.fn(), addTo: vi.fn() })),
  };
  return { __esModule: true, default: L };
});

let state: any;
vi.mock('../../store/index', () => ({
  useStore: (selector: any) => selector(state),
}));

import { Map as MapPage } from './Map';

describe('Map loading vs empty state', () => {
  beforeEach(() => {
    state = {
      config: null,
      connectionState: 'connected',
      peerLocations: [],
      status: null,
      peerNames: new globalThis.Map<number, string>(),
      routes: [],
      showRoutes: false,
      setShowRoutes: vi.fn(),
      mapFocusAddr: null,
      setMapFocusAddr: vi.fn(),
    };
  });

  it('renders a loading affordance when connected but config has not arrived yet', () => {
    render(<MapPage />);
    expect(screen.getByText('Loading map…')).toBeInTheDocument();
    expect(screen.queryByText(/GPS is disabled/)).not.toBeInTheDocument();
  });

  it('does not show loading text while disconnected and config is still null', () => {
    state.connectionState = 'disconnected';
    render(<MapPage />);
    expect(screen.queryByText('Loading map…')).not.toBeInTheDocument();
    expect(screen.getByText(/GPS is disabled/)).toBeInTheDocument();
  });

  it('renders the loaded-empty GPS-disabled message once config has arrived with GPS off and no peers', () => {
    state.config = { identity: { address: 1, name: 'Self' }, location: { enabled: false, default_tier: 'coarse', interval_s: 300, source: 'gps' } };
    render(<MapPage />);
    expect(screen.queryByText('Loading map…')).not.toBeInTheDocument();
    expect(screen.getByText(/GPS is disabled/)).toBeInTheDocument();
  });
});
