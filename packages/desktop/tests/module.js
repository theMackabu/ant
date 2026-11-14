import fs from 'node:fs';
import { registerHooks } from 'node:module';
import { fileURLToPath } from 'node:url';
const root = fileURLToPath(new URL('..', import.meta.url)).replace(/\/$/, '');
const calls = [];
const native = {
  versions: { ant: '1', desktop: '2', chrome: '3' }, resourcesPath: '/res',
  createWindow(json, config) { calls.push(['create', JSON.parse(json), config]); return calls.filter(c => c[0] === 'create').length; },
  load(id, url, rootDir) { calls.push(['load', id, url, rootDir]); queueMicrotask(() => native.dispatch('load', id)); },
  call(id, op, ...args) { calls.push(['call', id, op, ...args]); return op === 'getBounds' ? { x: 0, y: 0, width: 1, height: 2 } : op !== 'reload'; },
  send(...args) { calls.push(['send', ...args]); return true; },
  setMenu(json) { calls.push(['menu', json && JSON.parse(json)]); },
  path: name => (name === 'home' ? '/home' : undefined),
  quit() { calls.push(['quit']); }
};
const source = fs.readFileSync(root + '/js/clone.js', 'utf8') + '\n' + fs.readFileSync(root + '/js/main.js', 'utf8');
(0, eval)(fs.readFileSync(root + '/js/bootstrap.js', 'utf8'))(native, registerHooks, source);
const { app, BrowserWindow, ipcMain, Menu, MenuItem, versions } = await import('ant:desktop');
const assert = (c, m) => { if (!c) throw new Error(m); };
const throws = (f, text) => { try { f(); } catch (e) { assert(String(e.message).includes(text), 'wrong error: ' + e.message); return; } throw new Error('did not throw: ' + text); };
assert(!('__antDesktopNative' in globalThis), 'leak');
assert(app.ready() === app.ready() && app.versions === versions && versions.chrome === '3', 'app');
assert(app.getPath('home') === '/home', 'path'); throws(() => app.getPath('missing'), 'unknown app path');
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
const payload = calls.find(c => c[0] === 'send')[5];
await native.dispatch('ipc', 1, 1, 9, 'echo', payload);
await native.dispatch('ipc', 1, 1, 10, 'denied', payload);
await native.dispatch('ipc', 1, 0, 0, 'note', payload);
await native.dispatch('ipc', 1, 0, 0, 'echo', payload);
const replies = calls.filter(c => c[0] === 'send').slice(1);
assert(replies.length === 2 && replies[0][2] === 0 && replies[0][3] === 9 && replies[1][2] === 1 && replies[1][3] === 10, 'replies ' + JSON.stringify(replies.map(r => r.slice(0, 4))));
assert(noted.self === noted && noted.bytes[0] === 7 && noted.big === 9007199254740993n, 'clone');
throws(() => window.webContents.send('nope', 1), 'not granted');
throws(() => window.webContents.reload(), 'Chromium is not running');
assert(window.getBounds().height === 2, 'bounds'); throws(() => window.setTitle(1), 'requires a string');
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
