import os from 'node:os';
import { contextBridge, ipcRenderer } from 'ant:desktop/renderer';

contextBridge.exposeInMainWorld('desktop', {
  platform: os.platform(),
  runtimeInfo: () => ipcRenderer.invoke('app:get-runtime-info')
});

globalThis.preloadReady = true;
