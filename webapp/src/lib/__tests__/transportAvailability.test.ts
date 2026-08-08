import { describe, it, expect } from 'vitest';
import { describeTransports, DESKTOP_APP_URL, type GatedTransport, type TransportUnavailable } from '../transportAvailability';
import { LOCAL_LAN_UNAVAILABLE_REASON } from '../connectionMode';
import type { ConnectionCapabilities } from '../../types/bramble';

const HOSTED: ConnectionCapabilities = {
  mode: 'hosted',
  localLanAllowed: false,
  localLanReason: LOCAL_LAN_UNAVAILABLE_REASON,
};
const LOCAL: ConnectionCapabilities = { mode: 'local', localLanAllowed: true };

function unavailable(result: ReturnType<typeof describeTransports>, t: GatedTransport): TransportUnavailable {
  const entry = result[t];
  if (entry.available) throw new Error(`expected ${t} to be unavailable`);
  return entry;
}

describe('describeTransports availability', () => {
  it('marks all three available when the runtime and browser allow them', () => {
    const r = describeTransports({ capabilities: LOCAL, hasSerial: true, hasBluetooth: true, platform: 'desktop' });
    expect(r.serial.available).toBe(true);
    expect(r.ble.available).toBe(true);
    expect(r.wifi.available).toBe(true);
  });

  it('marks wifi unavailable in hosted mode even with serial and bluetooth present', () => {
    const r = describeTransports({ capabilities: HOSTED, hasSerial: true, hasBluetooth: true, platform: 'desktop' });
    expect(r.serial.available).toBe(true);
    expect(r.wifi.available).toBe(false);
  });
});

describe('describeTransports copy', () => {
  it('offers the desktop app for hosted wifi on desktop', () => {
    const r = describeTransports({ capabilities: HOSTED, hasSerial: true, hasBluetooth: true, platform: 'desktop' });
    const wifi = unavailable(r, 'wifi');
    expect(wifi.caption).toBe('Unavailable here');
    expect(wifi.body).toContain('insecure connection to a device on your home network');
    expect(wifi.body).toContain('The desktop app runs locally');
    expect(wifi.cta).toEqual({ label: 'Get the desktop app', href: DESKTOP_APP_URL });
  });

  it('omits the desktop CTA for hosted wifi on mobile', () => {
    for (const platform of ['android', 'ios'] as const) {
      const r = describeTransports({ capabilities: HOSTED, hasSerial: false, hasBluetooth: true, platform });
      const wifi = unavailable(r, 'wifi');
      expect(wifi.cta).toBeUndefined();
      expect(wifi.body).toContain('needs the desktop app on a computer');
    }
  });

  it('prefers a server-supplied localLanReason over the default wifi body', () => {
    const r = describeTransports({
      capabilities: { mode: 'hosted', localLanAllowed: false, localLanReason: 'Blocked by site policy.' },
      hasSerial: true,
      hasBluetooth: true,
      platform: 'desktop',
    });
    const wifi = unavailable(r, 'wifi');
    expect(wifi.body).toContain('Blocked by site policy.');
    expect(wifi.body).not.toContain('insecure connection to a device on your home network');
  });

  it('names Web Serial and offers the desktop app for USB on desktop', () => {
    const r = describeTransports({ capabilities: LOCAL, hasSerial: false, hasBluetooth: true, platform: 'desktop' });
    const usb = unavailable(r, 'serial');
    expect(usb.caption).toBe('Not in this browser');
    expect(usb.body).toContain('Web Serial');
    // No version floor: Web Serial has shipped in Chrome since 89, so naming a
    // recent release would understate the browsers that work.
    expect(usb.body).toContain('Chrome or Edge works, as does the desktop app.');
    expect(usb.cta).toEqual({ label: 'Get the desktop app', href: DESKTOP_APP_URL });
  });

  it('tells mobile users USB needs a computer', () => {
    const r = describeTransports({ capabilities: HOSTED, hasSerial: false, hasBluetooth: true, platform: 'android' });
    const usb = unavailable(r, 'serial');
    expect(usb.body).toContain('cannot talk to USB devices');
    expect(usb.cta).toBeUndefined();
  });

  it('names Web Bluetooth and offers the desktop app for BLE on desktop', () => {
    const r = describeTransports({ capabilities: LOCAL, hasSerial: true, hasBluetooth: false, platform: 'desktop' });
    const ble = unavailable(r, 'ble');
    expect(ble.body).toContain('Web Bluetooth');
    expect(ble.cta).toEqual({ label: 'Get the desktop app', href: DESKTOP_APP_URL });
  });

  it('points android users at Chrome without a desktop CTA', () => {
    const r = describeTransports({ capabilities: HOSTED, hasSerial: false, hasBluetooth: false, platform: 'android' });
    const ble = unavailable(r, 'ble');
    expect(ble.body).toContain('Chrome on Android');
    expect(ble.cta).toBeUndefined();
  });

  it('states the iOS dead end without implying an iOS app', () => {
    const r = describeTransports({ capabilities: HOSTED, hasSerial: false, hasBluetooth: false, platform: 'ios' });
    const ble = unavailable(r, 'ble');
    expect(ble.body).toContain('there is no iOS app');
    expect(ble.cta).toBeUndefined();
  });
});

describe('derived alternatives', () => {
  it('lists both remaining transports when two are available', () => {
    const r = describeTransports({ capabilities: HOSTED, hasSerial: true, hasBluetooth: true, platform: 'desktop' });
    expect(unavailable(r, 'wifi').alternatives).toBe('USB and Bluetooth work here.');
  });

  it('uses the singular verb for one remaining transport', () => {
    const r = describeTransports({ capabilities: HOSTED, hasSerial: false, hasBluetooth: true, platform: 'android' });
    expect(unavailable(r, 'wifi').alternatives).toBe('Bluetooth works here.');
  });

  it('says nothing works when nothing does, and points at the mock node', () => {
    const r = describeTransports({ capabilities: HOSTED, hasSerial: false, hasBluetooth: false, platform: 'ios' });
    const text = unavailable(r, 'ble').alternatives;
    expect(text).toContain('No transport in this browser can reach a node');
    expect(text).toContain('mock node');
    expect(text).toContain('Connect from a computer');
  });

  it('does not tell a desktop user to connect from a computer', () => {
    // Firefox and Safari on a desktop OS have neither Web Serial nor Web
    // Bluetooth, so hosted mode leaves all three unavailable. The body already
    // points at another browser and the desktop app, and the CTA repeats it,
    // so the mobile phrasing here would contradict the paragraph above it.
    const r = describeTransports({ capabilities: HOSTED, hasSerial: false, hasBluetooth: false, platform: 'desktop' });
    for (const t of ['serial', 'ble', 'wifi'] as const) {
      const entry = unavailable(r, t);
      expect(entry.alternatives).toContain('No transport in this browser can reach a node');
      expect(entry.alternatives).toContain('mock node');
      expect(entry.alternatives).not.toContain('Connect from a computer');
    }
  });

  it('never names a transport it marked unavailable', () => {
    const labels: Record<GatedTransport, string> = { serial: 'USB', ble: 'Bluetooth', wifi: 'WiFi' };
    const all: GatedTransport[] = ['serial', 'ble', 'wifi'];

    for (const hasSerial of [true, false]) {
      for (const hasBluetooth of [true, false]) {
        for (const capabilities of [HOSTED, LOCAL]) {
          for (const platform of ['desktop', 'android', 'ios'] as const) {
            const r = describeTransports({ capabilities, hasSerial, hasBluetooth, platform });
            for (const t of all) {
              const entry = r[t];
              if (entry.available) continue;
              for (const other of all) {
                if (r[other].available) continue;
                expect(entry.alternatives).not.toContain(`${labels[other]} works here`);
                expect(entry.alternatives).not.toContain(`${labels[other]} and`);
                expect(entry.alternatives).not.toContain(`and ${labels[other]}`);
              }
            }
          }
        }
      }
    }
  });
});
