import { resolve } from 'node:path';
import { defineConfig, externalizeDepsPlugin } from 'electron-vite';
import react from '@vitejs/plugin-react';
import { cspPlugin } from './csp.config';

export default defineConfig({
  main: {
    // bonjour-service and electron-updater are bundled (not externalized): the
    // packaged app ships no node_modules (electron-builder files excludes them),
    // so any dep the main process needs at runtime must be inlined here. Both
    // are pure JS, so bundling is safe.
    plugins: [externalizeDepsPlugin({ exclude: ['bonjour-service', 'electron-updater'] })],
    build: {
      outDir: 'out/main',
      rollupOptions: {
        input: {
          index: resolve(__dirname, 'electron/main.ts'),
        },
      },
    },
  },
  preload: {
    plugins: [externalizeDepsPlugin()],
    build: {
      outDir: 'out/preload',
      rollupOptions: {
        input: {
          index: resolve(__dirname, 'electron/preload.ts'),
        },
      },
    },
  },
  renderer: {
    root: '.',
    build: {
      outDir: 'out/renderer',
      rollupOptions: {
        input: {
          index: resolve(__dirname, 'index.html'),
        },
      },
    },
    plugins: [react(), cspPlugin()],
    define: {
      __APP_VERSION__: JSON.stringify(process.env.APP_VERSION || 'dev'),
    },
  },
});
