import { app, BrowserWindow, ipcMain, Menu, MenuItem } from 'ant:desktop';

await app.ready();
const menu = Menu.buildFromTemplate([
  { label: 'Ant', submenu: [{ role: 'quit', accelerator: 'CmdOrCtrl+Q' }] },
  { label: 'Edit', submenu: [{ role: 'copy' }, { role: 'paste' }] }
]);
menu.append(new MenuItem({ label: 'Window', submenu: [{ role: 'minimize' }] }));
menu.insert(1, new MenuItem({ label: 'File', submenu: [{ role: 'close' }] }));
Menu.setApplicationMenu(menu);
if (Menu.getApplicationMenu() !== menu || app.getApplicationMenu() !== menu) {
  throw new Error('application menu identity was not preserved');
}

const window = new BrowserWindow({
  width: 640,
  height: 400,
  titleBarStyle: 'hiddenInset',
  backgroundColor: '#120d26',
  borderColor: '#b778ff88',
  borderWidth: 1,
  webPreferences: {
    capabilities: [
      { channel: 'app:get-runtime-info', access: ['invoke'] },
      { channel: 'app:toggle-theme', access: ['invoke'] },
      { channel: 'page:ready', access: ['send'] },
      { channel: 'theme:changed', access: ['receive'] }
    ]
  }
});
const initialBounds = window.getBounds();
if (initialBounds.width !== 640 || initialBounds.height !== 400) {
  throw new Error(`unexpected BrowserWindow bounds: ${JSON.stringify(initialBounds)}`);
}
for (const name of ['x', 'y', 'width', 'height']) {
  if (!Number.isFinite(initialBounds[name])) throw new Error(`BrowserWindow bounds.${name} is invalid`);
}
window.on('move', () => {});
window.on('resize', () => {});
window.on('quit', () => {
  const bounds = window.getBounds();
  if (bounds.width !== 640 || bounds.height !== 400) throw new Error('BrowserWindow bounds changed before quit');
  console.log('browser-window-quit-ok');
});
ipcMain.handle('app:get-runtime-info', () => ({
  ant: process.version,
  platform: process.platform,
  rendererIsWindow: true
}));
ipcMain.handle('app:toggle-theme', () => ({ accent: '#b778ff' }));
let markRendererReady;
const rendererReady = new Promise(resolve => {
  markRendererReady = resolve;
});
ipcMain.on('page:ready', event => {
  event.sender.send('theme:changed', { accent: '#b778ff', sentAt: new Date() });
  markRendererReady();
});
const lifecycle = [];
for (const event of ['loading', 'navigation-start', 'navigation-commit', 'ready', 'title']) {
  window.on(event, value => lifecycle.push(value.type));
}
await window.loadFile(new URL('fixtures/browser-window-page.html', import.meta.url).pathname);
await Promise.race([
  rendererReady,
  new Promise((_, reject) => setTimeout(
    () => reject(new Error('example renderer script did not start')),
    3000
  ))
]);
if (!lifecycle.includes('navigation-start') || !lifecycle.includes('navigation-commit')) {
  throw new Error(`missing lifecycle events: ${lifecycle.join(', ')}`);
}
console.log('browser-window-smoke-ok');
if (process.env.ANT_DESKTOP_DEVTOOLS_SMOKE) {
  window.webContents.openDevTools();
  setTimeout(() => window.webContents.closeDevTools(), 1000);
}
setTimeout(() => app.quit(), process.env.ANT_DESKTOP_INPUT_SMOKE || process.env.ANT_DESKTOP_DEVTOOLS_SMOKE ? 1500 : 50);
