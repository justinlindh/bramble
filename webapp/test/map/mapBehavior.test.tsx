import { describe, it, expect, vi, beforeEach } from 'vitest';
import { render } from '@testing-library/react';

const fitBoundsMock = vi.fn();
const circleMarkerMock = vi.fn();
const rectangleMock = vi.fn();

const mapMock = {
  setView: vi.fn(function () { return mapMock; }),
  fitBounds: fitBoundsMock,
  remove: vi.fn(),
  removeLayer: vi.fn(),
};

const markerLikeLayer = () => {
  const layer: any = {
    bindPopup: vi.fn(() => layer),
    bindTooltip: vi.fn(() => layer),
    addTo: vi.fn(() => layer),
  };
  return layer;
};

vi.mock('leaflet', () => {
  const L = {
    Icon: { Default: { prototype: {}, mergeOptions: vi.fn() } },
    map: vi.fn(() => mapMock),
    tileLayer: vi.fn(() => ({ addTo: vi.fn() })),
    layerGroup: vi.fn(() => {
      const group = { addTo: vi.fn(() => group), clearLayers: vi.fn() };
      return group;
    }),
    latLng: vi.fn((lat: number, lon: number) => ({ lat, lng: lon })),
    latLngBounds: vi.fn(() => ({ pad: vi.fn(() => ({ mockedBounds: true })) })),
    circleMarker: vi.fn((...args: any[]) => {
      circleMarkerMock(...args);
      return markerLikeLayer();
    }),
    circle: vi.fn(() => markerLikeLayer()),
    rectangle: vi.fn((...args: any[]) => {
      rectangleMock(...args);
      return markerLikeLayer();
    }),
    polyline: vi.fn(() => markerLikeLayer()),
  };
  return { __esModule: true, default: L };
});

let state: any;
vi.mock('../../src/store/index', () => ({
  useStore: (selector: any) => selector(state),
}));

import { Map as MapPage } from '../../src/pages/Map/Map';

describe('Map behavior', () => {
  beforeEach(() => {
    fitBoundsMock.mockClear();
    circleMarkerMock.mockClear();
    rectangleMock.mockClear();
    state = {
      config: { identity: { address: 0x11111111, name: 'Self' }, location: { enabled: true, default_tier: 'coarse', interval_s: 300, source: 'gps' } },
      peerLocations: [
        { addr: 0x22222222, tier: 'full', position: { lat: 10, lon: 20, accuracy: 15 } },
      ],
      status: null,
      peerNames: new globalThis.Map<number, string>([[0x22222222, 'Peer 2']]),
      routes: [],
      showRoutes: false,
      setShowRoutes: vi.fn(),
      mapFocusAddr: null,
      setMapFocusAddr: vi.fn(),
    };
  });

  it('does not refit bounds when peer set is unchanged across polls', () => {
    const { rerender } = render(<MapPage />);
    expect(fitBoundsMock).toHaveBeenCalledTimes(1);

    state = {
      ...state,
      peerLocations: [
        { addr: 0x22222222, tier: 'full', position: { lat: 11, lon: 21, accuracy: 10 } },
      ],
    };

    rerender(<MapPage />);
    expect(fitBoundsMock).toHaveBeenCalledTimes(1);
  });

  it('does not claim the legend is sharing when the policy has no targets', () => {
    const { getByText } = render(<MapPage />);
    expect(getByText(/no targets, so nothing is published/i)).toBeTruthy();
  });

  it('reports the sharing cadence once a channel target exists', () => {
    state = {
      ...state,
      config: {
        ...state.config,
        location: { ...state.config.location, channel_targets: [{ channel: 0, enabled: true, tier: 'coarse', interval_s: 60 }] },
      },
    };

    const { getByText } = render(<MapPage />);
    expect(getByText(/Sharing coarse updates every 300s via gps/i)).toBeTruthy();
  });

  it('draws a coarse-tier peer as a zone rectangle around its quantized position', () => {
    state = {
      ...state,
      peerLocations: [
        { addr: 0x33333333, tier: 'coarse', position: { lat: 39.993, lon: -105.042, accuracy: 0 } },
      ],
      peerNames: new globalThis.Map<number, string>([[0x33333333, 'Zone Peer']]),
    };

    render(<MapPage />);
    expect(rectangleMock).toHaveBeenCalledTimes(1);
    const [bounds] = rectangleMock.mock.calls[0];
    expect(bounds[0][0]).toBeCloseTo(39.993, 9);
    expect(bounds[1][0]).toBeCloseTo(39.996, 9);
    expect(bounds[0][1]).toBeCloseTo(-105.043, 9);
    expect(bounds[1][1]).toBeCloseTo(-105.036, 9);
  });

  /* The node sends no coordinates for a presence share, so there is nothing
   * to place. The map used to key this tier on a grid-square string that no
   * firmware has ever sent, which drew a marker under the mock and nothing
   * at all against real hardware. */
  it('places nothing for a presence-tier peer', () => {
    state = {
      ...state,
      peerLocations: [
        { addr: 0x33333333, tier: 'presence', position: null },
      ],
      peerNames: new globalThis.Map<number, string>([[0x33333333, 'Presence Peer']]),
    };

    render(<MapPage />);
    expect(circleMarkerMock).not.toHaveBeenCalled();
    expect(rectangleMock).not.toHaveBeenCalled();
  });
});
