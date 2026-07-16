import { render, screen, fireEvent, act } from '@testing-library/react';
import { afterEach, describe, expect, it, vi } from 'vitest';
import { DevicePickerModal } from '../DevicePickerModal';
import type { DevicePickerRequest } from '../../types/desktop';

// The modal is driven entirely by the Electron preload bridge: a picker
// request renders the device list, clicking a row resolves it, and a null
// update (request settled in the main process) closes the modal.

function setupBridge() {
  let listener: ((req: DevicePickerRequest) => void) | null = null;
  const selectDevice = vi.fn();
  const cancelDevicePicker = vi.fn();
  window.brambleDesktop = {
    startDiscovery: vi.fn(),
    stopDiscovery: vi.fn(),
    onDiscovered: vi.fn(() => () => {}),
    onDevicePicker: vi.fn((cb: (req: DevicePickerRequest) => void) => {
      listener = cb;
      return () => { listener = null; };
    }),
    selectDevice,
    cancelDevicePicker,
    autoSelectNextDevice: vi.fn(),
  };
  return { push: (req: DevicePickerRequest) => act(() => listener?.(req)), selectDevice, cancelDevicePicker };
}

afterEach(() => {
  delete window.brambleDesktop;
});

describe('DevicePickerModal', () => {
  it('renders serial candidates and resolves the clicked one', () => {
    const { push, selectDevice } = setupBridge();
    render(<DevicePickerModal />);
    expect(screen.queryByRole('dialog')).toBeNull();

    push({ kind: 'serial', devices: [
      { id: 'p1', label: '/dev/ttyACM0', detail: '303a:1001' },
      { id: 'p2', label: '/dev/ttyUSB0', detail: '10c4:ea60' },
    ]});
    expect(screen.getByRole('dialog')).toBeTruthy();
    fireEvent.click(screen.getByText('/dev/ttyUSB0'));
    expect(selectDevice).toHaveBeenCalledWith('p2');
  });

  it('refreshes the bluetooth list live and closes on null', () => {
    const { push } = setupBridge();
    render(<DevicePickerModal />);
    push({ kind: 'bluetooth', devices: [] });
    expect(screen.getByText('No devices found yet')).toBeTruthy();
    push({ kind: 'bluetooth', devices: [{ id: 'd1', label: 'V4' }] });
    expect(screen.getByText('V4')).toBeTruthy();
    push(null);
    expect(screen.queryByRole('dialog')).toBeNull();
  });

  it('cancel dismisses via the bridge', () => {
    const { push, cancelDevicePicker } = setupBridge();
    render(<DevicePickerModal />);
    push({ kind: 'serial', devices: [{ id: 'p1', label: '/dev/ttyACM0' }] });
    fireEvent.click(screen.getByText('Cancel'));
    expect(cancelDevicePicker).toHaveBeenCalled();
  });

  it('exposes dialog role/aria-modal and Escape dismisses via the bridge cancel path', () => {
    const { push, cancelDevicePicker } = setupBridge();
    render(<DevicePickerModal />);
    push({ kind: 'serial', devices: [{ id: 'p1', label: '/dev/ttyACM0' }] });

    const dialog = screen.getByRole('dialog', { name: 'Select a serial device' });
    expect(dialog).toHaveAttribute('aria-modal', 'true');

    fireEvent.keyDown(screen.getByText('/dev/ttyACM0'), { key: 'Escape' });
    expect(cancelDevicePicker).toHaveBeenCalled();
  });

  it('backdrop click also dismisses via the bridge cancel path', () => {
    const { push, cancelDevicePicker } = setupBridge();
    render(<DevicePickerModal />);
    push({ kind: 'serial', devices: [{ id: 'p1', label: '/dev/ttyACM0' }] });

    fireEvent.click(screen.getByRole('dialog', { name: 'Select a serial device' }));
    expect(cancelDevicePicker).toHaveBeenCalled();
  });
});
