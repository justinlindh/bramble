// The saved-device book: persisting a connected device, and the list/forget/
// rename operations backing the device picker.
import { useStore } from '../index';
import { formatAddrHex } from '../../utils/address';
import { listDevices, forgetDevice, renameDevice, upsertDevice, setDeviceToken, clearDeviceToken } from '../../lib/deviceBook';

// Persist a device to the book once its real node address is known (post-connect).
// A book write must never break a live connection, so callers wrap this in the
// same try/catch used around the rest of the post-connect save path.
export function saveConnectedDevice(args: {
  addr: number;
  name?: string;
  ip: string;
  /**
   * Omit token AND remember when the connect flow had no credential controls
   * (serial): absence expresses no intent, so the stored token and the
   * entry's remember flag are preserved. Tokens are keyed by node address
   * across transports, so treating serial's hard-coded blanks as intent
   * wiped tokens the user remembered via wifi or BLE.
   */
  token?: string;
  remember?: boolean;
  transport: 'wifi' | 'serial' | 'ble';
  bleDeviceId?: string;
  bleDeviceName?: string;
}): void {
  const address = formatAddrHex(args.addr);
  upsertDevice({
    address, name: args.name, lastIp: args.ip, transport: args.transport, remember: args.remember,
    bleDeviceId: args.bleDeviceId, bleDeviceName: args.bleDeviceName,
  });
  if (args.token) {
    setDeviceToken(address, args.token, args.remember ?? false);
  } else if (args.remember === false) {
    // Blank token with Remember explicitly off: drop any stored copy.
    // Leaving it made a stale localStorage token survive the reconnect, so
    // "leave off on shared devices" did not actually take effect until the
    // entry was forgotten.
    clearDeviceToken(address);
  }
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
