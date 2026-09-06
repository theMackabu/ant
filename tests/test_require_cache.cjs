const assert = require('node:assert');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { createRequire } = require('node:module');
const tmp = fs.realpathSync(fs.mkdtempSync(path.join(os.tmpdir(), 'ant-require-cache-')));
const localRequire = createRequire(path.join(tmp, 'entry.cjs'));
const file = path.join(tmp, 'config.cjs');
const json = path.join(tmp, 'config.json');
try {
  assert.strictEqual(require.cache, localRequire.cache);
  assert.strictEqual(Object.getPrototypeOf(require.cache), null);
  fs.writeFileSync(file, 'module.exports = { version: 1, module, cache: require.cache };');
  const first = localRequire(file);
  assert.strictEqual(first.cache, require.cache);
  assert.strictEqual(require.cache[file], first.module);
  assert.strictEqual(require.cache[file].exports, first);
  assert.strictEqual(require.cache[file].loaded, true);
  assert.strictEqual(require.cache[file].filename, file);
  assert.strictEqual(require.cache[file].id, file);
  fs.writeFileSync(file, 'module.exports = { version: 2 };');
  assert.strictEqual(localRequire(file), first);
  // The same loop used by @tailwindcss/node/require-cache.
  for (const dependency of [file]) delete require.cache[dependency];
  assert.strictEqual(localRequire(file).version, 2);
  assert.strictEqual(first.version, 1);
  require.cache[file] = { exports: 42 };
  assert.strictEqual(localRequire(file), 42);
  delete require.cache[file];
  fs.writeFileSync(file, 'throw new Error("failed module");');
  assert.throws(() => localRequire(file), /failed module/);
  assert.strictEqual(Object.hasOwn(require.cache, file), false);
  fs.writeFileSync(file, 'module.exports = 3;');
  assert.strictEqual(localRequire(file), 3);

  const a = path.join(tmp, 'a.cjs');
  const b = path.join(tmp, 'b.cjs');
  fs.writeFileSync(a, 'exports.ready = false; exports.b = require("./b.cjs"); exports.ready = true;');
  fs.writeFileSync(b, 'const a = require("./a.cjs"); module.exports = { ready: a.ready, loaded: require.cache[require.resolve("./a.cjs")].loaded, a };');
  const cycle = localRequire(a);
  assert.strictEqual(cycle.b.ready, false);
  assert.strictEqual(cycle.b.loaded, false);
  assert.strictEqual(cycle.b.a, cycle);

  fs.writeFileSync(json, '{"version":1}');
  assert.strictEqual(localRequire(json).version, 1);
  assert.strictEqual(require.cache[json].loaded, true);
  fs.writeFileSync(json, '{"version":2}');
  assert.strictEqual(localRequire(json).version, 1);
  delete require.cache[json];
  assert.strictEqual(localRequire(json).version, 2);
  console.log('require.cache tests passed');
} finally {
  for (const name of Object.keys(require.cache)) {
    if (name.startsWith(tmp + path.sep)) delete require.cache[name];
  }
  fs.rmSync(tmp, { recursive: true, force: true });
}
