import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { render, screen, act, fireEvent } from '@testing-library/react';
import { NearbyNodes } from '../NearbyNodes';
import { useStore } from '../../store/index';
import type { DiscoveredNode } from '../../types/desktop';
import type { SavedDevice } from '../../lib/deviceBook';

vi.mock('../../store/actions', () => ({ connect: vi.fn() }));
import { connect } from '../../store/actions';

let discoveryCb: ((nodes: DiscoveredNode[]) => void) | null = null;

const garage: SavedDevice = {
  address: 'DEADBEEF', name: 'Node A', lastIp: '192.0.2.0',
  transport: 'wifi', remember: true, lastConnectedAt: 1,
};

beforeEach(() => {
  window.brambleDesktop = {
    startDiscovery: vi.fn(),
    stopDiscovery: vi.fn(),
    onDiscovered: vi.fn((cb) => { discoveryCb = cb; return () => { discoveryCb = null; }; }),
    onDevicePicker: vi.fn(() => () => {}),
    selectDevice: vi.fn(),
    cancelDevicePicker: vi.fn(),
    autoSelectNextDevice: vi.fn(),
    fetchOtaIndex: vi.fn(),
  };
  useStore.setState({ devices: [] });
  localStorage.clear();
});

afterEach(() => {
  delete window.brambleDesktop;
  vi.clearAllMocks();
});

describe('NearbyNodes', () => {
  it('renders nothing on web (no brambleDesktop bridge)', () => {
    delete window.brambleDesktop;
    const { container } = render(<NearbyNodes onPickUnknown={vi.fn()} />);
    expect(container.firstChild).toBeNull();
  });

  it('starts discovery on mount and stops on unmount', () => {
    const { unmount } = render(<NearbyNodes onPickUnknown={vi.fn()} />);
    expect(window.brambleDesktop!.startDiscovery).toHaveBeenCalledTimes(1);
    unmount();
    expect(window.brambleDesktop!.stopDiscovery).toHaveBeenCalledTimes(1);
  });

  it('one-click connects a saved node with token, current IP, and DHCP guard', () => {
    useStore.setState({ devices: [garage] });
    localStorage.setItem('bramble.deviceToken.DEADBEEF', 'sekrit');
    render(<NearbyNodes onPickUnknown={vi.fn()} />);
    act(() => discoveryCb!([
      { addrHex: 'DEADBEEF', name: 'Node A (fw)', hostname: 'bramble-6eee', ip: '192.0.2.0' },
    ]));
    fireEvent.click(screen.getByRole('button', { name: /connect to garage/i }));
    expect(connect).toHaveBeenCalledWith('wifi', expect.objectContaining({
      url: 'ws://192.0.2.0/ws',
      token: 'sekrit',
      ip: '192.0.2.0',
      remember: true,
      name: 'Node A',
      expectAddressHex: 'DEADBEEF',
    }));
  });

  it('hands unknown nodes to onPickUnknown instead of connecting', () => {
    const onPickUnknown = vi.fn();
    render(<NearbyNodes onPickUnknown={onPickUnknown} />);
    act(() => discoveryCb!([
      { addrHex: '11112222', name: 'Node B', hostname: 'bramble-2222', ip: '192.0.2.0' },
    ]));
    fireEvent.click(screen.getByRole('button', { name: /connect to attic/i }));
    expect(connect).not.toHaveBeenCalled();
    expect(onPickUnknown).toHaveBeenCalledWith(expect.objectContaining({
      ip: '192.0.2.0', txtName: 'Node B',
    }));
  });

  it('renders nothing while the snapshot is empty', () => {
    const { container } = render(<NearbyNodes onPickUnknown={vi.fn()} />);
    expect(container.firstChild).toBeNull();
  });
});
