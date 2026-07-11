import { app, BrowserWindow } from 'ant:desktop';

await app.ready();
const window = new BrowserWindow({ width: 640, height: 400, show: false });
await window.loadFile(new URL('fixtures/ipc-page.html', import.meta.url).pathname);

const opened = new Promise(resolve => window.on('devtools-opened', resolve));
window.webContents.openDevTools();
let openTimeout;
await Promise.race([opened, new Promise((_, reject) => {
  openTimeout = setTimeout(() => reject(new Error('Web Inspector did not open')), 3000);
})]);
clearTimeout(openTimeout);

window.on('quit', () => console.log('desktop-devtools-quit'));
setTimeout(() => app.quit(), 0);
