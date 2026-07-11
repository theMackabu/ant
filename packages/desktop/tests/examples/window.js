import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { BrowserWindow } from 'ant:desktop';

export const rendererEntry = path.join(
  path.dirname(fileURLToPath(import.meta.url)),
  'renderer',
  'index.html'
);

const preloadEntry = path.join(
  path.dirname(fileURLToPath(import.meta.url)),
  'preload.js'
);

export function createMainWindow() {
  const window = new BrowserWindow({
    width: 800,
    height: 520,
    title: 'Ant Desktop',
    titleBarStyle: 'hiddenInset',
    titleBarOverlay: { height: 48 },
    trafficLightPosition: { x: 16, y: 15 },
    backgroundColor: '#202124',
    webPreferences: {
      preload: preloadEntry,
      sandbox: false,
      nodeIntegration: true,
      contextIsolation: false,
      capabilities: [
        { channel: 'app:get-runtime-info', access: ['invoke'] },
        { channel: 'app:toggle-theme', access: ['invoke'] },
        { channel: 'page:ready', access: ['send'] }
      ]
    }
  });

  for (const event of [
    'navigation-start',
    'navigation-commit',
    'ready',
    'renderer-crash'
  ]) {
    window.on(event, value => {
      console.log(`window:${value.type}`, value.detail ?? '');
    });
  }

  return window;
}
