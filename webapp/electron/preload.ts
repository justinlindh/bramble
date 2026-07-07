import { contextBridge, ipcRenderer, type IpcRendererEvent } from 'electron';
import { electronAPI } from '@electron-toolkit/preload';
import { DISCOVERY_CHANNELS, type BrambleDesktopApi, type DiscoveredNode } from '../src/types/desktop';

const brambleDesktop: BrambleDesktopApi = {
  startDiscovery: (): void => { ipcRenderer.send(DISCOVERY_CHANNELS.start); },
  stopDiscovery: (): void => { ipcRenderer.send(DISCOVERY_CHANNELS.stop); },
  onDiscovered: (cb: (nodes: DiscoveredNode[]) => void): (() => void) => {
    const listener = (_event: IpcRendererEvent, nodes: DiscoveredNode[]) => cb(nodes);
    ipcRenderer.on(DISCOVERY_CHANNELS.update, listener);
    return () => { ipcRenderer.removeListener(DISCOVERY_CHANNELS.update, listener); };
  },
};

if (process.contextIsolated) {
  try {
    contextBridge.exposeInMainWorld('electron', electronAPI);
    contextBridge.exposeInMainWorld('isElectron', true);
    contextBridge.exposeInMainWorld('brambleDesktop', brambleDesktop);
  } catch (error) {
    console.error('Failed to expose electron API:', error);
  }
} else {
  // @ts-expect-error fallback for non-isolated context
  window.electron = electronAPI;
  // @ts-expect-error
  window.isElectron = true;
  window.brambleDesktop = brambleDesktop;
}
