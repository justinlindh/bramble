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
  token: string;
  remember: boolean;
  transport: 'wifi' | 'serial' | 'ble';
  bleDeviceId?: string;
  bleDeviceName?: string;
}): void {
  const address = formatAddrHex(args.addr);
  // Serial saves hard-code token '' and remember false because the serial
  // form has no token or Remember control, so that combination expresses no
  // intent about credentials. Tokens are keyed by node address across
  // transports: honoring it wiped the node's remembered wifi/BLE token (and
  // demoted its remember flag) on every USB connect, breaking the next
  // one-click row.
  const isSerial = args.transport === 'serial';
  const remember = isSerial
    ? (listDevices().find(d => d.address === address)?.remember ?? false)
    : args.remember;
  upsertDevice({
    address, name: args.name, lastIp: args.ip, transport: args.transport, remember,
    bleDeviceId: args.bleDeviceId, bleDeviceName: args.bleDeviceName,
  });
  if (args.token) {
    setDeviceToken(address, args.token, args.remember);
  } else if (!args.remember && !isSerial) {
    // Blank token with Remember off on a token-capable form: drop any stored
    // copy. Leaving it made a stale localStorage token survive the reconnect,
    // so "leave off on shared devices" did not actually take effect until the
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
