const createRendererBridge = bindings => {
  const grants = new Set(bindings.capabilities.split(';').filter(Boolean));
  const versions = Object.freeze({ ...bindings.versions });
  const pending = new Map();
  const listeners = new Map();
  let nextRequest = 1;

  const requireGrant = (access, channel) => {
    if (typeof channel !== 'string' || !grants.has(`${access}:${channel}`)) {
      throw new DOMException(`IPC capability not granted: ${access}:${channel}`, 'SecurityError');
    }
  };

  const ipc = Object.freeze({
    send(channel, value) {
      requireGrant('send', channel);
      bindings.send(0, 0, channel, encode(value));
    },

    invoke(channel, value) {
      requireGrant('invoke', channel);
      const id = nextRequest++;
      return new Promise((resolve, reject) => {
        pending.set(id, { resolve, reject });
        bindings.send(1, id, channel, encode(value));
      });
    },

    on(channel, listener) {
      requireGrant('receive', channel);
      if (typeof listener !== 'function') throw new TypeError('listener must be a function');
      const values = listeners.get(channel) || [];
      values.push(listener);
      listeners.set(channel, values);
      return () => {
        const index = values.indexOf(listener);
        if (index !== -1) values.splice(index, 1);
      };
    }
  });

  const expose = (name, value) => {
    if (typeof name !== 'string' || !name || name === 'Ant') {
      throw new TypeError('exposed API name must be a non-empty string other than Ant');
    }
    if (Object.prototype.hasOwnProperty.call(globalThis, name)) {
      throw new Error(`globalThis.${name} is already defined`);
    }
    Object.defineProperty(globalThis, name, { value, enumerable: true, configurable: false, writable: false });
  };

  const hidden = (name, value) =>
    Object.defineProperty(globalThis, name, { value, enumerable: false, configurable: false, writable: false });

  const modules = new Map([['ant:desktop/renderer', Object.freeze({ contextBridge: Object.freeze({ exposeInMainWorld: expose }), ipcRenderer: ipc })]]);

  const unwrap = (json, label) => {
    const response = JSON.parse(json);
    if (!response.ok) throw new Error(label ? `${label}: ${response.error}` : response.error);
    return response.value;
  };

  const require = specifier => {
    if (typeof specifier !== 'string') throw new TypeError('require() needs one node: or ant: module specifier');
    let module = modules.get(specifier);
    if (module) return module;
    if (bindings.sandbox) throw new Error(`${specifier} requires webPreferences.sandbox: false`);
    module = {};
    for (const entry of unwrap(bindings.describe(specifier))) {
      module[entry.name] = entry.callable
        ? (...args) => unwrap(bindings.call(specifier, entry.name, JSON.stringify(args)), `${specifier}.${entry.name}`)
        : entry.value;
    }
    modules.set(specifier, Object.freeze(module));
    return module;
  };

  if (bindings.integration) {
    const { arch, cwd, platform } = require('node:process');
    hidden('require', require);
    hidden('process', Object.freeze({ arch, cwd, platform, versions }));
  }
  hidden('Ant', Object.freeze({ expose, ipc, versions }));

  return Object.freeze({
    require,
    receive(operation, id, channel, payload) {
      if (operation === 2) {
        if (!grants.has(`receive:${channel}`)) return;
        const value = decode(payload);
        for (const listener of [...(listeners.get(channel) || [])]) listener(value);
        return;
      }
      const request = pending.get(id);
      if (!request) return;
      pending.delete(id);
      (operation === 0 ? request.resolve : request.reject)(decode(payload));
    }
  });
};
