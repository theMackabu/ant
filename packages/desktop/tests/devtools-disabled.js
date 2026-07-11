import { app, BrowserWindow } from 'ant:desktop';

await app.ready();
const window = new BrowserWindow({ width: 640, height: 400, show: false });
await window.loadFile(new URL('fixtures/ipc-page.html', import.meta.url).pathname);

window.webContents.openDevTools();
window.webContents.toggleDevTools();
window.webContents.inspectElement(0, 0);
await new Promise(resolve => setTimeout(resolve, 500));

if (window.webContents.isDevToolsOpened()) {
  throw new Error('Web Inspector opened outside development mode');
}

console.log('desktop-devtools-disabled');
window.close();
