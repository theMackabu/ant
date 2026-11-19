import fs from 'node:fs';
import path from 'node:path';
import v8 from 'node:v8';
import native from 'ant:internal/desktop';

const NOT_RUNNING = 'Chromium is not running for this BrowserWindow';
const ACCESS = ['send', 'invoke', 'receive'];
const WINDOW_OPTION_TYPES = ['boolean', 'number', 'string'];

const windows = new Map();
const handlers = new Map();
const listeners = new Map();
const menuClicks = new Map();
let applicationMenu = null;
let readiness;

export const versions = native.versions;

function grantsFrom(capabilities) {
  const grants = new Set();
  if (capabilities === undefined) return grants;
  if (!Array.isArray(capabilities)) throw new TypeError('webPreferences.capabilities must be an array');
  for (const capability of capabilities) {
    const channel = capability?.channel;
    if (typeof channel !== 'string' || !/^[A-Za-z0-9:._-]{1,128}$/.test(channel) || !Array.isArray(capability.access)) {
      throw new TypeError('each IPC capability needs a safe channel and access array');
    }
    for (const access of capability.access) {
      if (typeof access !== 'string') throw new TypeError('IPC access values must be strings');
      if (!ACCESS.includes(access)) throw new TypeError('IPC access must be send, invoke, or receive');
      grants.add(`${access}:${channel}`);
    }
  }
  return grants;
}

function integrationFrom(preferences) {
  const { nodeIntegration, antIntegration } = preferences;
  if (nodeIntegration !== undefined && typeof nodeIntegration !== 'boolean') {
    throw new TypeError('webPreferences.nodeIntegration must be a boolean');
  }
  if (antIntegration !== undefined && typeof antIntegration !== 'boolean') {
    throw new TypeError('webPreferences.antIntegration must be a boolean');
  }
  if (nodeIntegration !== undefined && antIntegration !== undefined && nodeIntegration !== antIntegration) {
    throw new TypeError('webPreferences.antIntegration and nodeIntegration must match');
  }
  return antIntegration ?? nodeIntegration ?? false;
}

function preloadFrom(preferences) {
  const { preload } = preferences;
  if (preload === undefined) return '';
  if (typeof preload !== 'string') throw new TypeError('webPreferences.preload must be a file path');
  const resolved = path.resolve(preload);
  if (!fs.statSync(resolved, { throwIfNoEntry: false })?.isFile()) {
    throw new Error(`preload file does not exist: ${resolved}`);
  }
  return resolved;
}

function nativeOptions(options) {
  const result = {};
  for (const [name, value] of Object.entries(options)) {
    if (WINDOW_OPTION_TYPES.includes(typeof value)) result[name] = value;
  }
  const position = options.trafficLightPosition;
  if (position && typeof position === 'object') {
    result.trafficLightPosition = {
      x: typeof position.x === 'number' ? position.x : 12,
      y: typeof position.y === 'number' ? position.y : 12
    };
  }
  return JSON.stringify(result);
}

export class BrowserWindow {
  #id = 0;
  #grants;
  #events = new Map();
  #load = null;

  constructor(options) {
    if (!options || typeof options !== 'object') options = {};
    const preferences = options.webPreferences && typeof options.webPreferences === 'object' ? options.webPreferences : {};
    const sandbox = typeof preferences.sandbox === 'boolean' ? preferences.sandbox : true;
    const integration = integrationFrom(preferences);
    if (sandbox && integration) throw new TypeError('webPreferences.nodeIntegration requires sandbox: false');
    const preload = preloadFrom(preferences);
    this.#grants = grantsFrom(preferences.capabilities);

    this.#id = native.createWindow(nativeOptions(options), {
      capabilities: [...this.#grants].join(';'),
      preload,
      sandbox,
      integration,
      contextIsolation: typeof preferences.contextIsolation === 'boolean' ? preferences.contextIsolation : true,
      transparent: options.transparent === true || typeof options.vibrancy === 'string'
    });
    windows.set(this.#id, this);

    const call = (operation, ...args) => {
      if (!native.call(this.#id, operation, ...args)) throw new Error(NOT_RUNNING);
    };
    this.webContents = Object.freeze({
      openDevTools: () => call('openDevTools'),
      closeDevTools: () => call('closeDevTools'),
      toggleDevTools: () => call('toggleDevTools'),
      reload: () => call('reload'),
      isDevToolsOpened: () => native.call(this.#id, 'isDevToolsOpened') === true,
      inspectElement: (x, y) => {
        if (typeof x !== 'number' || typeof y !== 'number') throw new TypeError('inspectElement(x, y) requires coordinates');
        call('inspect', x, y);
      },
      send: (channel, value) => {
        if (typeof channel !== 'string') throw new TypeError('webContents.send(channel, value) requires a channel');
        if (!this.#grants.has(`receive:${channel}`)) throw new Error(`IPC receive channel is not granted: ${channel}`);
        if (!native.send(this.#id, 0, v8.serialize([channel, value]))) throw new Error(NOT_RUNNING);
      }
    });
    if (preferences.capabilities !== undefined) this.preloadCapabilities = preferences.capabilities;
  }

  static _dispatch(window, kind, a, b, c) {
    if (kind === 'event') return window.#emit(a, b, c);
    if (kind === 'ipc') return window.#receive(a, b);
    if (kind === 'load') {
      const load = window.#load;
      window.#load = null;
      if (a === undefined) load?.resolve();
      else load?.reject(new Error(a));
      return;
    }
    windows.delete(window.#id);
    window.#id = 0;
  }

  #emit(type, detail, code) {
    const event = { type, code };
    if (detail) event.detail = detail;
    for (const listener of [...(this.#events.get(type) || [])]) {
      try {
        listener.call(this, event);
      } catch (error) {
        console.error(`desktop ${type} listener failed:`, error);
      }
    }
  }

  async #receive(query, payload) {
    const event = Object.freeze({ sender: this.webContents, senderFrame: null });
    const reply = (ok, value) => this.#id && native.send(this.#id, query, v8.serialize([ok, value]));
    try {
      const [access, channel, value] = v8.deserialize(payload);
      if (!this.#grants.has(`${access}:${channel}`)) throw new Error(`IPC ${access} channel is not granted: ${channel}`);
      if (access === 'send') {
        reply(true);
        for (const listener of listeners.get(channel) || []) listener(event, value);
        return;
      }
      const handler = handlers.get(channel);
      if (!handler) throw new Error(`No ipcMain handler registered for ${channel}`);
      reply(true, await handler(event, value));
    } catch (error) {
      reply(false, error instanceof Error ? error : new Error(String(error)));
    }
  }

  #call(operation, ...args) {
    if (!this.#id) throw new Error('BrowserWindow is closed');
    return native.call(this.#id, operation, ...args);
  }

  #open(url, root) {
    if (!this.#id) throw new Error('BrowserWindow is closed');
    return new Promise((resolve, reject) => {
      native.load(this.#id, url, root);
      this.#load = { resolve, reject };
    });
  }

  loadURL(url) {
    if (typeof url !== 'string') throw new TypeError('loadURL(url) requires a string');
    return this.#open(url, '');
  }

  loadFile(file) {
    if (typeof file !== 'string') throw new TypeError('loadFile(path) requires a string');
    const resolved = path.resolve(file);
    if (!fs.existsSync(resolved)) throw new Error(`file does not exist: ${resolved}`);
    return this.#open(`ant://app/${encodeURIComponent(path.basename(resolved))}`, path.dirname(resolved));
  }

  on(event, listener) {
    if (typeof event !== 'string' || typeof listener !== 'function') {
      throw new TypeError('BrowserWindow.on(event, listener) requires a name and function');
    }
    const values = this.#events.get(event) || [];
    values.push(listener);
    this.#events.set(event, values);
    return this;
  }

  getBounds() {
    return this.#call('getBounds');
  }

  close() {
    this.#call('close');
  }

  show() {
    this.#call('show');
  }

  hide() {
    this.#call('hide');
  }

  minimize() {
    this.#call('minimize');
  }

  restore() {
    this.#call('restore');
  }

  maximize() {
    this.#call('maximize');
  }

  setAlwaysOnTop(flag) {
    if (typeof flag !== 'boolean') throw new TypeError('setAlwaysOnTop(flag) requires a boolean');
    this.#call('setAlwaysOnTop', flag);
  }

  setTitle(title) {
    if (typeof title !== 'string') throw new TypeError('setTitle(title) requires a string');
    this.#call('setTitle', title);
  }

  setFullScreen(flag) {
    if (typeof flag !== 'boolean') throw new TypeError('setFullScreen(flag) requires a boolean');
    this.#call('setFullScreen', flag);
  }
}

const dispatchWindow = BrowserWindow._dispatch;
delete BrowserWindow._dispatch;

native.dispatch = (kind, id, a, b, c) => {
  if (kind === 'menu') return menuClicks.get(id)?.();
  const window = windows.get(id);
  if (window) return dispatchWindow(window, kind, a, b, c);
};

function channelName(channel) {
  return typeof channel === 'string' && channel.length > 0;
}

export const ipcMain = {
  handle(channel, handler) {
    if (!channelName(channel) || typeof handler !== 'function') {
      throw new TypeError('ipcMain.handle(channel, handler) requires a non-empty channel and function');
    }
    if (handlers.has(channel)) throw new Error(`an IPC handler is already registered for ${channel}`);
    handlers.set(channel, handler);
  },
  removeHandler(channel) {
    if (!channelName(channel)) throw new TypeError('ipcMain.removeHandler(channel) requires a non-empty channel');
    handlers.delete(channel);
  },
  on(channel, listener) {
    if (!channelName(channel) || typeof listener !== 'function') {
      throw new TypeError('ipcMain.on(channel, listener) requires a non-empty channel and function');
    }
    const values = listeners.get(channel) || [];
    values.push(listener);
    listeners.set(channel, values);
  }
};

export class MenuItem {
  constructor(options) {
    if (options && typeof options === 'object') Object.assign(this, options);
  }
}

class MenuInstance {
  constructor(items) {
    this.items = items;
  }

  append(item) {
    if (!item || typeof item !== 'object') throw new TypeError('menu.append(item) requires a MenuItem');
    this.items.push(item);
    if (applicationMenu === this) showMenu(this.items);
  }

  insert(position, item) {
    if (typeof position !== 'number' || !item || typeof item !== 'object') {
      throw new TypeError('menu.insert(position, item) requires an index and MenuItem');
    }
    this.items.splice(Math.min(Math.max(0, Math.trunc(position)), this.items.length), 0, item);
    if (applicationMenu === this) showMenu(this.items);
  }
}

function describeMenu(items) {
  const nodes = [];
  for (const item of items) {
    if (!item || typeof item !== 'object') continue;
    const node = {};
    for (const name of ['label', 'role', 'type', 'accelerator']) {
      if (typeof item[name] === 'string') node[name] = item[name];
    }
    for (const name of ['enabled', 'visible', 'checked']) {
      if (typeof item[name] === 'boolean') node[name] = item[name];
    }
    if (typeof item.click === 'function') {
      node.id = menuClicks.size + 1;
      menuClicks.set(node.id, item.click);
    }
    const submenu = item.submenu instanceof MenuInstance ? item.submenu.items : item.submenu;
    if (Array.isArray(submenu)) node.submenu = describeMenu(submenu);
    nodes.push(node);
  }
  return nodes;
}

function showMenu(items) {
  menuClicks.clear();
  native.setMenu(items ? JSON.stringify(describeMenu(items)) : null);
}

function setApplicationMenu(menu) {
  if (menu === null || menu === undefined) {
    applicationMenu = null;
    showMenu(null);
    return;
  }
  const items = Array.isArray(menu) ? menu : menu instanceof MenuInstance ? menu.items : null;
  if (!items) throw new TypeError('setApplicationMenu(menu) requires a Menu or template array');
  applicationMenu = menu;
  showMenu(items);
}

function getApplicationMenu() {
  return applicationMenu;
}

export const Menu = {
  buildFromTemplate(template) {
    if (!Array.isArray(template)) throw new TypeError('Menu.buildFromTemplate(template) requires an array');
    return new MenuInstance(template);
  },
  setApplicationMenu,
  getApplicationMenu
};

export const app = {
  resourcesPath: native.resourcesPath,
  versions,
  ready: () => (readiness ??= Promise.resolve()),
  quit: () => native.quit(),
  getPath(name) {
    if (typeof name !== 'string') throw new TypeError('app.getPath(name) requires a string');
    const value = native.path(name);
    if (value === undefined) throw new Error(`unknown app path: ${name}`);
    return value;
  },
  setApplicationMenu,
  getApplicationMenu
};
