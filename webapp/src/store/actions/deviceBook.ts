// The saved-device book: persisting a connected device, and the list/forget/
// rename operations backing the device picker.
import { useStore } from '../index';
import { formatAddrHex } from '../../utils/address';
import { listDevices, forgetDevice, renameDevice, upsertDevice, setDeviceToken } from '../../lib/deviceBook';

// Persist a device to the book once its real node address is known (post-connect).
// A book write must never break a live connection, so callers wrap this in the
// same try/catch used around the rest of the post-connect save path.
export function saveConnectedDevice(args: {
  addr: number;
  name?: string;
  ip: string;
  token: string;
  remember: boolean;
  transport: 'wifi' | 'serial' | 'ble';
  bleDeviceId?: string;
  bleDeviceName?: string;
}): void {
  const address = formatAddrHex(args.addr);
  upsertDevice({
    address, name: args.name, lastIp: args.ip, transport: args.transport, remember: args.remember,
    bleDeviceId: args.bleDeviceId, bleDeviceName: args.bleDeviceName,
  });
  if (args.token) setDeviceToken(address, args.token, args.remember);
  refreshDevices();
}

export function refreshDevices(): void {
  useStore.getState().setDevices(listDevices());
}
export function forgetSavedDevice(address: string): void {
  forgetDevice(address);
  refreshDevices();
}
export function renameSavedDevice(address: string, name: string): void {
  renameDevice(address, name);
  refreshDevices();
}
