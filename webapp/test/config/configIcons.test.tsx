import { describe, expect, it, vi } from 'vitest';
import { render, screen } from '@testing-library/react';
import { Config } from '../../src/pages/Config/Config';

const mockState: any = {
  config: {
    identity: {},
    radio: {},
    channels: [],
    location: {},
  },
  status: null,
  neighbors: [],
  routes: [],
};

vi.mock('../../src/store/index', () => ({
  useStore: (selector: any) => selector(mockState),
}));

vi.mock('../../src/store/messageDb', () => ({
  messageDb: {
    clearAll: vi.fn(async () => {}),
  },
}));

vi.mock('../../src/pages/Config/IdentitySection', () => ({ IdentitySection: () => <div /> }));
vi.mock('../../src/pages/Config/RadioForm', () => ({ RadioForm: () => <div /> }));
vi.mock('../../src/pages/Config/ChannelManager', () => ({ ChannelManager: () => <div /> }));
vi.mock('../../src/pages/Config/PeerManager', () => ({ PeerManager: () => <div /> }));
vi.mock('../../src/pages/Config/LocationSection', () => ({ LocationSection: () => <div /> }));
vi.mock('../../src/pages/Config/TrafficDebugSection', () => ({ TrafficDebugSection: () => <div /> }));
vi.mock('../../src/pages/Config/NetworkKeySection', () => ({ NetworkKeySection: () => <div /> }));

describe('Config section icons', () => {
  it('uses a warning icon for Traffic Debug and a non-warning icon for Data', () => {
    const warningPath = 'M10.29 3.86L1.82 18a2 2 0 0 0 1.71 3h16.94a2 2 0 0 0 1.71-3L13.71 3.86a2 2 0 0 0-3.42 0z';

    render(<Config />);

    const trafficHeading = screen.getByRole('heading', { name: /traffic debug/i });
    const dataHeading = screen.getByRole('heading', { name: /^data$/i });

    expect(trafficHeading.querySelector(`path[d="${warningPath}"]`)).toBeTruthy();
    expect(dataHeading.querySelector(`path[d="${warningPath}"]`)).toBeNull();
  });
});
