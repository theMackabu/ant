import { app, ipcMain, versions } from 'ant:desktop';

if (typeof app.resourcesPath !== 'string' || !app.resourcesPath) {
  throw new Error('app.resourcesPath is unavailable');
}
for (const name of ['home', 'temp', 'appData', 'userData', 'desktop', 'documents', 'downloads', 'resources', 'exe']) {
  const value = app.getPath(name);
  if (typeof value !== 'string' || !value) throw new Error(`app.getPath('${name}') is unavailable`);
}
try {
  app.getPath('missing');
  throw new Error('app.getPath accepted an unknown path');
} catch (error) {
  if (!String(error).includes('unknown app path')) throw error;
}
for (const name of ['ant', 'desktop', 'chrome']) {
  if (typeof versions[name] !== 'string' || !versions[name]) {
    throw new Error(`versions.${name} is unavailable`);
  }
}
if (process.versions['ant-desktop'] !== versions.desktop ||
    process.versions.chrome !== versions.chrome) {
  throw new Error('desktop component versions are missing from process.versions');
}
await app.ready();
ipcMain.handle('smoke:ping', value => value);
ipcMain.removeHandler('smoke:ping');
ipcMain.handle('smoke:ping', value => value);
console.log('desktop-smoke-ok');
setTimeout(() => app.quit(), 10);
