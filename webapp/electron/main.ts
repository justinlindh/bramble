import { app, BrowserWindow, ipcMain, Menu, session, shell } from 'electron';
import { join } from 'node:path';
import { is } from '@electron-toolkit/utils';
import { startDiscovery, stopDiscovery } from './discovery';
import { DEVICE_PICKER_CHANNELS, DISCOVERY_CHANNELS, type PickerDevice } from '../src/types/desktop';

let mainWindow: BrowserWindow | null = null;

// Chromium on Linux ships Web Bluetooth behind a feature flag; without this
// navigator.bluetooth does not exist and the webapp disables its BLE UI.
app.commandLine.appendSwitch('enable-features', 'WebBluetooth');

// Pending chooser state: Electron fires select-serial-port once per request
// and select-bluetooth-device REPEATEDLY as scanning discovers devices. The
// active callback resolves the request; each event refreshes the list shown
// by the renderer's picker modal.
let pendingPicker: ((deviceId: string) => void) | null = null;
// One-shot expected device for a saved-device reconnect: when the chooser
// fires and a candidate matches, resolve silently instead of showing the
// picker modal. Cleared on use or when the renderer disarms it.
let autoSelectExpected: { id?: string; name?: string } | null = null;

function sendPickerUpdate(kind: 'serial' | 'bluetooth', devices: PickerDevice[]): void {
  mainWindow?.webContents.send(DEVICE_PICKER_CHANNELS.update, { kind, devices });
}

function resolvePicker(deviceId: string): void {
  const cb = pendingPicker;
  pendingPicker = null;
  mainWindow?.webContents.send(DEVICE_PICKER_CHANNELS.update, null);
  cb?.(deviceId);
}

function createWindow(): void {
  mainWindow = new BrowserWindow({
    width: 1024,
    height: 768,
    minWidth: 400,
    minHeight: 600,
    title: 'Bramble',
    icon: join(__dirname, '../../public/bramble-logo.png'),
    webPreferences: {
      preload: join(__dirname, '../preload/index.js'),
      contextIsolation: true,
      nodeIntegration: false,
    },
  });

  session.defaultSession.setPermissionCheckHandler((_webContents, permission) => {
    if (permission === 'serial' || permission === 'bluetooth') {
      return true;
    }
    return false;
  });

  // No auto-picking: both handlers forward the candidates to an in-app
  // picker so the user chooses. Auto-selecting by vendor id silently
  // connected to the wrong node when two Bramble devices were attached.
  session.defaultSession.on('select-serial-port', (event, portList, _webContents, callback) => {
    event.preventDefault();
    if (portList.length === 0) {
      callback('');
      return;
    }
    pendingPicker = callback;
    sendPickerUpdate('serial', portList.map(p => ({
      id: p.portId,
      label: p.displayName && p.displayName !== p.portName ? `${p.displayName} (${p.portName})` : p.portName,
      detail: p.vendorId ? `${Number(p.vendorId).toString(16).padStart(4, '0')}:${Number(p.productId ?? 0).toString(16).padStart(4, '0')}` : undefined,
    })));
  });

  // NOTE the asymmetry: select-serial-port is a session event, but
  // select-bluetooth-device is a webContents event. Registering it on the
  // session compiles fine and never fires, so Electron's default handler
  // cancelled every Web Bluetooth request instantly.
  mainWindow.webContents.on('select-bluetooth-device', (event, devices, callback) => {
    event.preventDefault();
    if (autoSelectExpected) {
      const match = devices.find(d =>
        (autoSelectExpected!.id && d.deviceId === autoSelectExpected!.id) ||
        (autoSelectExpected!.name && d.deviceName === autoSelectExpected!.name)
      );
      if (match) {
        autoSelectExpected = null;
        callback(match.deviceId);
        return;
      }
      // No match yet: keep scanning silently; the renderer's connect
      // timeout is the fallback if the saved device never appears.
      pendingPicker = callback;
      return;
    }
    // Fires again as scanning finds more devices: keep the newest callback
    // and refresh the list; the request stays open until the user picks.
    pendingPicker = callback;
    sendPickerUpdate('bluetooth', devices.map(d => ({
      id: d.deviceId,
      label: d.deviceName || d.deviceId,
    })));
  });

  ipcMain.on(DEVICE_PICKER_CHANNELS.select, (_event, id: string) => {
    if (typeof id === 'string' && id.length > 0) resolvePicker(id);
  });
  ipcMain.on(DEVICE_PICKER_CHANNELS.cancel, () => {
    resolvePicker('');
  });
  ipcMain.on(DEVICE_PICKER_CHANNELS.autoSelect, (_event, expected: { id?: string; name?: string } | null) => {
    autoSelectExpected = expected;
  });

  session.defaultSession.setDevicePermissionHandler((details) => {
    if (details.deviceType === 'serial' || details.deviceType === 'usb' || details.deviceType === 'hid' || details.deviceType === 'bluetooth') {
      return true;
    }
    return false;
  });

  mainWindow.webContents.on('before-input-event', (_event, input) => {
    if (input.type !== 'keyDown') return;
    if (input.key === 'F12' || (input.control && input.shift && input.key.toLowerCase() === 'i')) {
      mainWindow?.webContents.toggleDevTools();
    }
  });

  mainWindow.webContents.setWindowOpenHandler(({ url }) => {
    if (url.startsWith('http')) {
      shell.openExternal(url);
    }
    return { action: 'deny' };
  });

  if (is.dev && process.env['ELECTRON_RENDERER_URL']) {
    mainWindow.loadURL(process.env['ELECTRON_RENDERER_URL']);
  } else {
    mainWindow.loadFile(join(__dirname, '../renderer/index.html'));
  }

  mainWindow.setTitle(`Bramble ${app.getVersion()}`);

  mainWindow.on('closed', () => {
    stopDiscovery();
    mainWindow = null;
  });
}

app.whenReady().then(() => {
  // No application menu: the app is a single-page companion and the
  // Bramble/Edit/View/Help bar reads as browser chrome. Clipboard shortcuts
  // work natively in Chromium on Linux/Windows without menu accelerators;
  // DevTools stays reachable below for debugging.
  Menu.setApplicationMenu(null);
  createWindow();

  ipcMain.on(DISCOVERY_CHANNELS.start, (event) => {
    startDiscovery((nodes) => {
      if (!event.sender.isDestroyed()) {
        event.sender.send(DISCOVERY_CHANNELS.update, nodes);
      }
    });
  });
  ipcMain.on(DISCOVERY_CHANNELS.stop, () => stopDiscovery());

  app.on('activate', () => {
    if (BrowserWindow.getAllWindows().length === 0) {
      createWindow();
    }
  });
});

app.on('window-all-closed', () => {
  if (process.platform !== 'darwin') {
    app.quit();
  }
});
