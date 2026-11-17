import fs from 'node:fs';
import { registerHooks } from 'node:module';
import v8 from 'node:v8';
import { fileURLToPath } from 'node:url';
const root = fileURLToPath(new URL('..', import.meta.url)).replace(/\/$/, '');
const calls = [];
const native = {
  versions: { ant: '1', desktop: '2', chrome: '3' }, resourcesPath: '/res',
  createWindow(json, config) { calls.push(['create', JSON.parse(json), config]); return calls.filter(c => c[0] === 'create').length; },
  load(id, url, rootDir) { calls.push(['load', id, url, rootDir]); queueMicrotask(() => native.dispatch('load', id)); },
  call(id, op, ...args) { calls.push(['call', id, op, ...args]); return op !== 'reload'; },
  send(...args) { calls.push(['send', ...args]); return true; },
  setMenu(json) { calls.push(['menu', json && JSON.parse(json)]); },
  quit() { calls.push(['quit']); }
};
const sources = {
  'ant:desktop': fs.readFileSync(root + '/src/builtins/desktop.js', 'utf8'),
  'ant:internal/desktop': 'export default globalThis.__antDesktopNative; delete globalThis.__antDesktopNative;'
};
globalThis.__antDesktopNative = native;
registerHooks({
  resolve: (specifier, context, next) => (specifier in sources ? { url: specifier, format: 'module', shortCircuit: true } : next(specifier, context)),
  load: (url, context, next) => (url in sources ? { format: 'module', source: sources[url], shortCircuit: true } : next(url, context))
});
const { app, BrowserWindow, ipcMain, Menu, MenuItem, versions } = await import('ant:desktop');
const assert = (c, m) => { if (!c) throw new Error(m); };
const throws = (f, text) => { try { f(); } catch (e) { assert(String(e.message).includes(text), 'wrong error: ' + e.message); return; } throw new Error('did not throw: ' + text); };
assert(!('__antDesktopNative' in globalThis), 'leak');
assert(app.ready() === app.ready() && app.versions === versions, 'app');
throws(() => new BrowserWindow({ webPreferences: { antIntegration: true } }), 'requires sandbox: false');
throws(() => new BrowserWindow({ webPreferences: { sandbox: false, nodeIntegration: true, antIntegration: false } }), 'must match');
throws(() => new BrowserWindow({ webPreferences: { capabilities: [{ channel: 'bad channel', access: ['send'] }] } }), 'safe channel');
const window = new BrowserWindow({ width: 640, show: false, vibrancy: 'sidebar', trafficLightPosition: { x: 3 }, webPreferences: { capabilities: [
  { channel: 'echo', access: ['invoke'] }, { channel: 'note', access: ['send'] }, { channel: 'event', access: ['receive'] }] } });
const created = calls.find(c => c[0] === 'create');
assert(created[1].width === 640 && created[1].show === false && created[1].trafficLightPosition.y === 12 && !('webPreferences' in created[1]), 'options');
assert(created[2].capabilities === 'invoke:echo;send:note;receive:event' && created[2].sandbox && created[2].transparent && created[2].contextIsolation, 'config');
await window.loadFile(root + '/tests/fixtures/ipc-page.html');
assert(calls.find(c => c[0] === 'load')[2] === 'ant://app/ipc-page.html', 'url');
let seen = []; window.on('title', e => seen.push(e)).on('title', function () { seen.push(this === window); });
native.dispatch('event', 1, 'title', 'Hello', 0);
assert(seen[0].type === 'title' && seen[0].detail === 'Hello' && seen[1] === true, 'events');
ipcMain.handle('echo', (event, value) => { assert(event.sender === window.webContents, 'sender'); return value; });
throws(() => ipcMain.handle('echo', () => {}), 'already registered');
let noted; ipcMain.on('note', (_e, v) => (noted = v));
const cyclic = { bytes: new Uint8Array([7]), big: 9007199254740993n }; cyclic.self = cyclic;
window.webContents.send('event', cyclic);
const sends = () => calls.filter(c => c[0] === 'send');
const [eventChannel, eventValue] = v8.deserialize(sends()[0][3]);
assert(sends()[0][2] === 0 && eventChannel === 'event' && eventValue.self === eventValue, 'event message');
const request = (access, channel) => v8.serialize([access, channel, cyclic]);
await native.dispatch('ipc', 1, 9, request('invoke', 'echo'));
await native.dispatch('ipc', 1, 10, request('invoke', 'denied'));
await native.dispatch('ipc', 1, 11, request('send', 'note'));
await native.dispatch('ipc', 1, 12, Buffer.from('garbage'));
const replies = sends().slice(1).map(c => [c[2], ...v8.deserialize(c[3])]);
assert(replies.length === 4, 'reply count ' + replies.length);
assert(replies[0][0] === 9 && replies[0][1] === true && replies[0][2].self === replies[0][2], 'invoke reply');
assert(replies[1][0] === 10 && replies[1][1] === false && /not granted/.test(replies[1][2].message), 'denied reply');
assert(replies[2][0] === 11 && replies[2][1] === true, 'send reply');
assert(replies[3][0] === 12 && replies[3][1] === false, 'garbage reply');
assert(noted.self === noted && noted.bytes[0] === 7 && noted.big === 9007199254740993n, 'clone');
throws(() => window.webContents.send('nope', 1), 'not granted');
throws(() => window.webContents.reload(), 'Chromium is not running');
throws(() => window.setTitle(1), 'requires a string');
let clicked = 0;
const menu = Menu.buildFromTemplate([{ label: 'A', submenu: [{ role: 'quit' }, { label: 'Go', click: () => clicked++ }] }]);
Menu.setApplicationMenu(menu); menu.append(new MenuItem({ label: 'W' })); menu.insert(1, new MenuItem({ label: 'F' }));
assert(Menu.getApplicationMenu() === menu && app.getApplicationMenu() === menu, 'menu identity');
const shown = calls.filter(c => c[0] === 'menu').at(-1)[1];
assert(shown.map(i => i.label).join() === 'A,F,W' && shown[0].submenu[1].id === 1, 'menu ' + JSON.stringify(shown));
native.dispatch('menu', 1); assert(clicked === 1, 'click');
native.dispatch('destroyed', 1); throws(() => window.show(), 'BrowserWindow is closed');
app.quit(); assert(calls.at(-1)[0] === 'quit', 'quit');
console.log('desktop-module-ok');
