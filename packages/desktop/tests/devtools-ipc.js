import { app, BrowserWindow, ipcMain } from 'ant:desktop';

await app.ready();

const results = new Map();
ipcMain.handle('devtools:echo', (_event, value) => `echo:${value}`);
ipcMain.on('devtools:result', (_event, value) => results.set(value.step, value));

const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
const roundTrip = async step => {
  if (step !== 'loaded') window.webContents.send('devtools:go', step);
  for (let attempt = 0; attempt < 80 && !results.has(step); attempt++) await sleep(50);
  const result = results.get(step);
  if (!result) throw new Error(`no IPC round trip ${step} DevTools`);
  if (step !== 'loaded' && result.reply !== `echo:${step}`) throw new Error(`wrong reply ${step} DevTools: ${JSON.stringify(result)}`);
};

const window = new BrowserWindow({
  width: 640,
  height: 400,
  show: false,
  webPreferences: {
    capabilities: [
      { channel: 'devtools:echo', access: ['invoke'] },
      { channel: 'devtools:result', access: ['send'] },
      { channel: 'devtools:go', access: ['receive'] }
    ]
  }
});
await window.loadFile(new URL('fixtures/devtools-ipc-page.html', import.meta.url).pathname);

await roundTrip('loaded');
await roundTrip('before');

const opened = new Promise(resolve => window.on('devtools-opened', resolve));
window.webContents.openDevTools();
await opened;
await sleep(1500);
await roundTrip('with');

const closed = new Promise(resolve => window.on('devtools-closed', resolve));
window.webContents.closeDevTools();
await closed;
await sleep(250);
await roundTrip('after');

console.log('desktop-devtools-ipc-ok');
window.close();
