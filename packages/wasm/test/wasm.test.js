import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import test from 'node:test';

import { Ant, AntDisposedError, AntTimeoutError } from '../dist/index.js';

test('ships the reviewed reactor import and export contract', async () => {
  const module = await WebAssembly.compile(await readFile(new URL('../dist/ant.wasm', import.meta.url)));
  assert.deepEqual(WebAssembly.Module.imports(module), [
    { module: 'env', name: 'memory', kind: 'memory' },
    { module: 'ant', name: 'now_ms', kind: 'function' },
    { module: 'ant', name: 'host_call', kind: 'function' },
    { module: 'ant', name: 'random_fill', kind: 'function' },
    { module: 'wasi_snapshot_preview1', name: 'environ_get', kind: 'function' },
    { module: 'wasi_snapshot_preview1', name: 'environ_sizes_get', kind: 'function' },
    { module: 'wasi_snapshot_preview1', name: 'fd_close', kind: 'function' },
    { module: 'wasi_snapshot_preview1', name: 'fd_prestat_get', kind: 'function' },
    { module: 'wasi_snapshot_preview1', name: 'fd_prestat_dir_name', kind: 'function' },
    { module: 'wasi_snapshot_preview1', name: 'fd_seek', kind: 'function' },
    { module: 'wasi_snapshot_preview1', name: 'fd_write', kind: 'function' },
    { module: 'wasi_snapshot_preview1', name: 'proc_exit', kind: 'function' },
    { module: 'wasi_snapshot_preview1', name: 'random_get', kind: 'function' }
  ]);
  assert.deepEqual(
    WebAssembly.Module.exports(module).map(item => item.name),
    ['_initialize', 'ant_alloc', 'ant_free', 'ant_create', 'ant_destroy', 'ant_result_length', 'ant_release_result', 'ant_set_global', 'ant_eval']
  );
});

test('evaluates Silver and preserves instance state', async () => {
  const ant = await Ant.create();
  try {
    assert.deepEqual(await ant.eval('Array.from({ length: 5 }, (_, i) => i ** 2)'), [0, 1, 4, 9, 16]);
    await ant.eval('globalThis.answer = 42');
    assert.equal(await ant.eval('answer'), 42);
  } finally {
    ant.dispose();
  }
});

test('isolates globals and memory between instances', async () => {
  const [left, right] = await Promise.all([Ant.create({ memoryLimit: 32 * 1024 * 1024 }), Ant.create({ memoryLimit: 64 * 1024 * 1024 })]);
  try {
    await left.eval("globalThis.side = 'left'");
    await right.eval("globalThis.side = 'right'");
    assert.equal(await left.eval('side'), 'left');
    assert.equal(await right.eval('side'), 'right');
  } finally {
    left.dispose();
    right.dispose();
  }
});

test('transfers supported values without JSON number loss', async () => {
  const ant = await Ant.create();
  try {
    const value = await ant.eval('({ u: undefined, b: 123n, n: NaN, i: Infinity, z: -0 })');
    assert.equal(value.u, undefined);
    assert.equal(value.b, 123n);
    assert.ok(Number.isNaN(value.n));
    assert.equal(value.i, Infinity);
    assert.ok(Object.is(value.z, -0));
  } finally {
    ant.dispose();
  }
});

test('preserves lone surrogates and exact data properties', async () => {
  const loneSurrogates = '\ud800value\udfff';
  const input = Object.create(null);
  Object.defineProperty(input, '__proto__', {
    enumerable: true,
    value: { changed: true }
  });
  const globals = Object.create(null);
  globals.input = input;
  globals.loneSurrogates = loneSurrogates;
  Object.defineProperty(globals, '__proto__', {
    enumerable: true,
    value: { changed: true }
  });

  const ant = await Ant.create({ globals });
  try {
    assert.equal(await ant.eval('loneSurrogates'), loneSurrogates);
    assert.deepEqual(
      await ant.eval(`({
      inputOwn: Object.prototype.hasOwnProperty.call(input, "__proto__"),
      inputPrototypeClean: Object.getPrototypeOf(input) === Object.prototype,
      globalOwn: Object.prototype.hasOwnProperty.call(globalThis, "__proto__"),
      globalPrototypeClean: Object.getPrototypeOf(globalThis) !== globalThis.__proto__,
    })`),
      {
        inputOwn: true,
        inputPrototypeClean: true,
        globalOwn: true,
        globalPrototypeClean: true
      }
    );
  } finally {
    ant.dispose();
  }
});

test('keeps rooted values alive across repeated guest collections', async () => {
  const ant = await Ant.create({
    memoryLimit: 64 * 1024 * 1024,
    timeout: 5_000
  });
  try {
    assert.equal(
      await ant.eval(`
      globalThis.keep = Array.from(
        { length: 4_000 },
        (_, i) => ({ i, text: String(i) }),
      );
      for (let round = 0; round < 25; round++) {
        Array.from(
          { length: 4_000 },
          (_, i) => ({ round, i, text: String(i) }),
        );
      }
      keep[3_999].text
    `),
      '3999'
    );
  } finally {
    ant.dispose();
  }
});

test('enforces the per-instance WebAssembly memory ceiling', async () => {
  const ant = await Ant.create({ memoryLimit: 32 * 1024 * 1024 });
  try {
    await assert.rejects(
      ant.eval(`
        globalThis.keep = [];
        while (true) keep.push({ text: "x".repeat(1_024) });
      `),
      /oom|memory/i
    );
  } finally {
    ant.dispose();
  }
});

test('calls synchronous host globals and makes host failures catchable', async () => {
  const ant = await Ant.create({
    globals: {
      greet: name => `Hello, ${name}!`,
      config: { enabled: true },
      fail: () => {
        throw new Error('host failed');
      }
    }
  });
  try {
    assert.equal(await ant.eval("greet('Silver')"), 'Hello, Silver!');
    assert.equal(await ant.eval('config.enabled'), true);
    assert.equal(await ant.eval('try { fail() } catch (error) { error.message }'), 'host failed');
  } finally {
    ant.dispose();
  }
});

test('normalizes hostile host exceptions without escaping the Wasm import', async () => {
  const hostile = Object.create(null);
  for (const name of ['name', 'message', 'stack']) {
    Object.defineProperty(hostile, name, {
      get() {
        throw new Error(`cannot read ${name}`);
      }
    });
  }
  hostile[Symbol.toPrimitive] = () => {
    throw new Error('cannot stringify');
  };

  const ant = await Ant.create({
    globals: {
      fail: () => {
        throw hostile;
      }
    }
  });
  try {
    assert.equal(await ant.eval('try { fail() } catch (error) { error.message }'), 'Host function failed');
    assert.equal(await ant.eval('6 * 7'), 42);
  } finally {
    ant.dispose();
  }
});

test('drains Promise reactions and resolved await continuations', async () => {
  const ant = await Ant.create();
  try {
    await ant.eval(`
      globalThis.promiseValue = 0;
      Promise.resolve(21).then(value => { promiseValue = value * 2 });
      0;
    `);
    assert.equal(await ant.eval('promiseValue'), 42);

    await ant.eval(`
      globalThis.awaitValue = 0;
      async function update() {
        await 1;
        awaitValue = 7;
      }
      update();
      0;
    `);
    assert.equal(await ant.eval('awaitValue'), 7);
    await assert.rejects(ant.eval('Promise.resolve(1)'), /cannot be transferred/);
  } finally {
    ant.dispose();
  }
});

test('interrupts runaway bytecode even inside guest try/catch', async () => {
  const ant = await Ant.create({ timeout: 10 });
  try {
    await assert.rejects(
      ant.eval("try { while (true) {} } catch (error) { 'escaped' }"),
      error => error instanceof AntTimeoutError && error.message === 'Execution timed out'
    );
  } finally {
    ant.dispose();
  }
});

test('keeps timeout authority through result transfer', async () => {
  const ant = await Ant.create({ timeout: 10 });
  try {
    await assert.rejects(ant.eval('({ get value() { while (true) {} } })'), AntTimeoutError);
    await assert.rejects(ant.eval(`throw new Error("Execution timed out")`), error => error instanceof Error && !(error instanceof AntTimeoutError));
    assert.deepEqual(await ant.eval('Object.keys = () => { while (true) {} }; ({ value: 42 })'), { value: 42 });
  } finally {
    ant.dispose();
  }
});

test('reports guest errors and rejects use after disposal', async () => {
  const ant = await Ant.create();
  await assert.rejects(ant.eval("throw new SyntaxError('bad source')"), error => error instanceof SyntaxError && error.message === 'bad source');
  ant[Symbol.dispose]();
  await assert.rejects(ant.eval('1'), AntDisposedError);
  ant.dispose();
});

test('validates limits and boundary inputs', async () => {
  await assert.rejects(Ant.create({ memoryLimit: 1024 }), /memoryLimit must be an integer/);
  await assert.rejects(Ant.create({ timeout: -1 }), /timeout must be an integer/);

  const ant = await Ant.create();
  try {
    await assert.rejects(ant.eval('one\0two'), /source cannot contain NUL/);
    await assert.rejects(ant.eval('(() => { const value = {}; value.self = value; return value })()'), /cannot be transferred/);
    await assert.rejects(ant.eval('(() => { const value = []; value.length = 1048577; return value })()'), /cannot be transferred/);
    await assert.rejects(ant.eval("({ get value() { throw new Error('getter failed') } })"), /cannot be transferred/);
    assert.equal(await ant.eval(`"x".replace(/(x)/, "$0-$9-$1")`), '$0-$9-x');
    assert.equal(await ant.eval('21 * 2'), 42);
  } finally {
    ant.dispose();
  }
});
