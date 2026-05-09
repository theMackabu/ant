const assert = require('node:assert');
const events = require('node:events');

globalThis.onunhandledrejection = () => {};

assert.strictEqual(typeof events, 'function');
assert.strictEqual(typeof events.once, 'function');
assert.strictEqual(typeof events.on, 'function');
assert.strictEqual(typeof events.addAbortListener, 'function');
assert.strictEqual(typeof events.setMaxListeners, 'function');
assert.strictEqual(typeof events.getMaxListeners, 'function');

{
  class CompatibleEmitter {
    constructor() {
      this.listenersByName = Object.create(null);
    }

    emit(name, value) {
      const listener = this.listenersByName[name];
      delete this.listenersByName[name];
      listener?.(value);
    }

    once(name, fn) {
      this.listenersByName[name] = fn;
      return this;
    }

    on(name, fn) {
      return this.once(name, fn);
    }

    addListener(name, fn) {
      return this.on(name, fn);
    }

    removeListener(name, fn) {
      if (this.listenersByName[name] === fn) delete this.listenersByName[name];
      return this;
    }

    removeAllListeners() {
      return this;
    }

    listeners() {
      return [];
    }

    rawListeners() {
      return [];
    }

    listenerCount(name) {
      if (name === undefined) return Object.keys(this.listenersByName).length;
      return this.listenersByName[name] ? 1 : 0;
    }

    eventNames() {
      return [];
    }
  }

  const compatible = new CompatibleEmitter();
  const ready = events.once(compatible, 'ready');
  compatible.emit('ready', 99);
  ready.then(([value]) => {
    assert.strictEqual(value, 99);
  });

  const controller = new AbortController();
  const waiting = events.once(compatible, 'hello', { signal: controller.signal });
  assert.strictEqual(compatible.listenerCount('hello'), 1);
  controller.abort();

  waiting.then(
    () => assert.fail('aborted events.once should reject'),
    () => {
      assert.strictEqual(compatible.listenerCount('hello'), 0);
      console.log('node:events-cjs-static-helpers:ok');
    }
  );
}

{
  const lookups = Object.create(null);

  class CachedCompatibleEmitter {
    constructor() {
      this.listenersByName = Object.create(null);
    }
  }

  function install(name, fn) {
    Object.defineProperty(CachedCompatibleEmitter.prototype, name, {
      configurable: true,
      get() {
        lookups[name] = (lookups[name] || 0) + 1;
        return fn;
      },
    });
  }

  install('once', function once(name, fn) {
    this.listenersByName[name] = fn;
    return this;
  });
  install('on', function on(name, fn) {
    return this.once(name, fn);
  });
  install('emit', function emit(name, value) {
    const listener = this.listenersByName[name];
    delete this.listenersByName[name];
    listener?.(value);
    return !!listener;
  });
  install('addListener', function addListener(name, fn) {
    return this.on(name, fn);
  });
  install('removeListener', function removeListener(name, fn) {
    if (this.listenersByName[name] === fn) delete this.listenersByName[name];
    return this;
  });
  install('removeAllListeners', function removeAllListeners() {
    this.listenersByName = Object.create(null);
    return this;
  });
  install('listeners', function listeners() {
    return [];
  });
  install('rawListeners', function rawListeners() {
    return [];
  });
  install('listenerCount', function listenerCount(name) {
    if (name === undefined) return Object.keys(this.listenersByName).length;
    return this.listenersByName[name] ? 1 : 0;
  });
  install('eventNames', function eventNames() {
    return Object.keys(this.listenersByName);
  });

  const countLookups = () => Object.values(lookups).reduce((total, count) => total + count, 0);

  const first = new CachedCompatibleEmitter();
  const firstReady = events.once(first, 'ready');
  const firstLookupCount = countLookups();

  const second = new CachedCompatibleEmitter();
  const secondReady = events.once(second, 'ready');
  const secondLookupDelta = countLookups() - firstLookupCount;

  if (globalThis.Ant) {
    assert(secondLookupDelta <= 2, `expected cached EventEmitter-like check, got ${secondLookupDelta} lookups`);
  }

  first.emit('ready', 1);
  second.emit('ready', 2);

  Promise.all([firstReady, secondReady]).then(([[a], [b]]) => {
    assert.strictEqual(a, 1);
    assert.strictEqual(b, 2);
  });
}
