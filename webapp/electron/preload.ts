import { contextBridge } from 'electron';
import { electronAPI } from '@electron-toolkit/preload';

if (process.contextIsolated) {
  try {
    contextBridge.exposeInMainWorld('electron', electronAPI);
    contextBridge.exposeInMainWorld('isElectron', true);
  } catch (error) {
    console.error('Failed to expose electron API:', error);
  }
} else {
  // @ts-expect-error fallback for non-isolated context
  window.electron = electronAPI;
  // @ts-expect-error
  window.isElectron = true;
}
