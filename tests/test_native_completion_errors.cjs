const assert = require('node:assert');
const { EventEmitter } = require('node:events');

function throwsExactly(fn, reason, label) {
  let caught = false;
  try { fn(); } catch (error) { caught = true; assert.strictEqual(error, reason, label); }
  assert.ok(caught, `${label}: missing throw`);
}

for (const reason of [undefined, null, new Error('original')]) {
  const emitter = new EventEmitter();
  let laterCalls = 0;
  emitter.once('event', () => { throw reason; });
  emitter.on('event', () => laterCalls++);
  throwsExactly(() => emitter.emit('event'), reason, 'emit');
  assert.strictEqual(laterCalls, 0);
  assert.strictEqual(emitter.emit('event'), true);
  assert.strictEqual(laterCalls, 1, 'once listener must be removed even after throwing');

  for (const Ctor of [Map, Set, WeakMap, WeakSet, Headers, Blob]) {
    for (const stage of ['open', 'next', 'done', 'value']) {
      let closed = 0;
      const source = {
        [Symbol.iterator]() { return this; },
        next() {
          if (stage === 'next') throw reason;
          return {
            get done() { if (stage === 'done') throw reason; return false; },
            get value() { throw reason; },
          };
        },
        return() { closed++; return {}; },
      };
      if (stage === 'open') Object.defineProperty(source, Symbol.iterator, { get() { throw reason; } });
      throwsExactly(() => new Ctor(source), reason, `${Ctor.name} ${stage}`);
      assert.strictEqual(closed, 0, 'a failed iterator step must not call return');
    }
  }

  for (const stage of ['next', 'done', 'value']) {
    let mapped = 0;
    const source = { next() { return {
      get done() { if (stage === 'done') throw reason; return false; },
      get value() { throw reason; },
    }; } };
    if (stage === 'next') Object.defineProperty(source, 'next', { get() { throw reason; } });
    throwsExactly(() => Iterator.prototype.map.call(source, () => mapped++).next(), reason, `iterator ${stage}`);
    assert.strictEqual(mapped, 0);
  }

  for (const field of ['wasClean', 'code', 'reason']) {
    const init = {};
    Object.defineProperty(init, field, { get() { throw reason; } });
    throwsExactly(() => new CloseEvent('close', init), reason, `CloseEvent ${field}`);
  }
  let reasonRead = false;
  throwsExactly(() => new CloseEvent('close', {
    code: { valueOf() { throw reason; } },
    get reason() { reasonRead = true; return ''; },
  }), reason, 'CloseEvent code conversion');
  assert.strictEqual(reasonRead, false);
  throwsExactly(() => new CloseEvent('close', { reason: { toString() { throw reason; } } }), reason, 'CloseEvent reason conversion');
}

for (const globalName of ['RegExp', 'String']) {
  const descriptor = Object.getOwnPropertyDescriptor(globalThis, globalName);
  let reads = 0;
  Object.defineProperty(globalThis, globalName, { configurable: true, get() { reads++; throw new Error('guard getter'); } });
  try {
    assert.strictEqual(/x/.exec('x')[0], 'x');
    assert.strictEqual('x'.replace(/x/, 'y'), 'y');
    assert.strictEqual(reads, 0, 'optimization guard invoked a global getter');
  } finally { Object.defineProperty(globalThis, globalName, descriptor); }
}
queueMicrotask(() => console.log('Native completion errors ok'));
