import { fileURLToPath } from 'node:url';
import { app, BrowserWindow, ipcMain } from 'ant:desktop';

await app.ready();

let resolveCompleted;
let rejectCompleted;
const completed = new Promise((resolve, reject) => {
  resolveCompleted = resolve;
  rejectCompleted = reject;
});

ipcMain.on('node:done', (_event, value) => {
  if (value.directImport && value.genericBuiltin && value.antBuiltin && value.preloadPrivileged && value.sharedContext && value.globals) {
    resolveCompleted();
  } else {
    rejectCompleted(new Error(`invalid node integration result: ${JSON.stringify(value)}`));
  }
});
ipcMain.on('node:error', (_event, value) => rejectCompleted(new Error(value.message)));

const window = new BrowserWindow({
  show: false,
  webPreferences: {
    preload: fileURLToPath(new URL('fixtures/node-preload.js', import.meta.url)),
    sandbox: false,
    antIntegration: true,
    contextIsolation: false,
    capabilities: [
      { channel: 'node:done', access: ['send'] },
      { channel: 'node:error', access: ['send'] }
    ]
  }
});

await window.loadFile(fileURLToPath(new URL('fixtures/node-page.html', import.meta.url)));
await Promise.race([
  completed,
  new Promise((_, reject) => setTimeout(
    () => reject(new Error('desktop node integration smoke timed out')),
    5000
  ))
]);
console.log('desktop-node-integration-smoke-ok');
window.close();
