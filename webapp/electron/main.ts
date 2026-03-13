import { app, BrowserWindow, Menu, session, shell } from 'electron';
import { join } from 'node:path';
import { is } from '@electron-toolkit/utils';

let mainWindow: BrowserWindow | null = null;

function createMenu(): void {
  const template: Electron.MenuItemConstructorOptions[] = [
    {
      label: 'Bramble',
      submenu: [
        { role: 'about' },
        { type: 'separator' },
        {
          label: 'Check for Updates...',
          click: () => {
            shell.openExternal('https://bramblemesh.org/downloads');
          },
        },
        { type: 'separator' },
        { role: 'quit' },
      ],
    },
    {
      label: 'Edit',
      submenu: [
        { role: 'undo' },
        { role: 'redo' },
        { type: 'separator' },
        { role: 'cut' },
        { role: 'copy' },
        { role: 'paste' },
        { role: 'selectAll' },
      ],
    },
    {
      label: 'View',
      submenu: [
        { role: 'reload' },
        { role: 'forceReload' },
        { role: 'toggleDevTools' },
        { type: 'separator' },
        { role: 'resetZoom' },
        { role: 'zoomIn' },
        { role: 'zoomOut' },
        { type: 'separator' },
        { role: 'togglefullscreen' },
      ],
    },
    {
      label: 'Help',
      submenu: [
        {
          label: 'Bramble Documentation',
          click: () => shell.openExternal('https://bramblemesh.org/docs'),
        },
        {
          label: 'Report Issue',
          click: () => shell.openExternal('https://github.com/bramble/bramble/issues'),
        },
      ],
    },
  ];

  Menu.setApplicationMenu(Menu.buildFromTemplate(template));
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

  session.defaultSession.on('select-serial-port', (event, portList, _webContents, callback) => {
    event.preventDefault();
    if (portList.length === 1) {
      callback(portList[0].portId);
      return;
    }
    const knownVendors = [0x303A, 0x10C4, 0x1A86, 0x0403];
    const match = portList.find(p => knownVendors.includes(p.vendorId));
    if (match) {
      callback(match.portId);
      return;
    }
    if (portList.length > 0) {
      callback(portList[0].portId);
    } else {
      callback('');
    }
  });

  session.defaultSession.on('select-bluetooth-device', (event, devices, callback) => {
    event.preventDefault();
    if (devices.length > 0) {
      callback(devices[0].deviceId);
    } else {
      callback('');
    }
  });

  session.defaultSession.setDevicePermissionHandler((details) => {
    if (details.deviceType === 'serial' || details.deviceType === 'usb' || details.deviceType === 'hid' || details.deviceType === 'bluetooth') {
      return true;
    }
    return false;
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
    mainWindow = null;
  });
}

app.whenReady().then(() => {
  createMenu();
  createWindow();

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
