const assert = require('node:assert');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const Module = require('node:module');

class DerivedModule extends Module {}
assert(new DerivedModule('derived.cjs') instanceof DerivedModule);
for (const id of [null, 1, false, {}]) assert.throws(() => new Module(id), TypeError);
assert.strictEqual(new Module().filename, null);
assert.strictEqual(Object.hasOwn(new Module(), 'paths'), false);

const tmp = fs.realpathSync(fs.mkdtempSync(path.join(os.tmpdir(), 'ant-cache-cleanup-')));
const requireFromTmp = Module.createRequire(path.join(tmp, 'entry.cjs'));
const originalCache = Module._cache;
const file = path.join(tmp, 'loaded.cjs');
const counter = '__antCacheCleanupLoads';
try {
  fs.writeFileSync(file, `globalThis.${counter} = (globalThis.${counter} || 0) + 1; module.exports = 42;`);
  assert.deepStrictEqual(requireFromTmp.resolve.paths('./child'), [tmp]);
  assert.deepStrictEqual(requireFromTmp.resolve.paths('../child'), [tmp]);
  assert.strictEqual(requireFromTmp.resolve.paths('node:fs'), null);

  const esm = path.join(tmp, 'side-effect.mjs');
  fs.writeFileSync(esm, `globalThis.${counter} = 1; export default 1;`);
  Module._cache = Object.freeze({});
  assert.throws(() => requireFromTmp(esm), TypeError);
  assert.strictEqual(globalThis[counter], undefined, 'cache failure must precede ESM execution');
  const sentinel = new Error('cache set failed');
  for (const cache of [Object.freeze({}), new Proxy({}, { set() { throw sentinel; } })]) {
    Module._cache = cache;
    assert.throws(() => requireFromTmp(file));
    assert.strictEqual(globalThis[counter], undefined, 'cache failure must precede module execution');
  }
  const inheritedReadOnly = Object.create(null);
  Object.defineProperty(inheritedReadOnly, file, { value: undefined, writable: false });
  Module._cache = Object.create(inheritedReadOnly);
  assert.throws(() => requireFromTmp(file), TypeError);
  assert.strictEqual(globalThis[counter], undefined);

  let publications = 0;
  let published;
  const inheritedSetter = Object.create(null);
  Object.defineProperty(inheritedSetter, file, { set(value) { publications++; published = value; } });
  Module._cache = Object.preventExtensions(Object.create(inheritedSetter));
  assert.strictEqual(requireFromTmp(file), 42);
  assert.strictEqual(published.exports, 42);
  assert.strictEqual(published.loaded, true);
  assert.strictEqual(publications, 1);

  Module._cache = originalCache;
  const wrapper = path.join(tmp, 'wrapper.cjs');
  fs.writeFileSync(wrapper, 'module.exports = (options) => require.resolve("fixture", options);');
  const good = path.join(tmp, 'good');
  const bad = path.join(tmp, 'bad');
  fs.mkdirSync(path.join(good, 'node_modules', 'fixture'), { recursive: true });
  fs.mkdirSync(bad);
  const target = path.join(good, 'node_modules', 'fixture', 'index.js');
  fs.writeFileSync(target, 'module.exports = 1;');
  const options = { paths: [bad, good] };
  assert.strictEqual(requireFromTmp.resolve('fixture', options), target);
  assert.strictEqual(requireFromTmp(wrapper)(options), target);
  assert.throws(() => requireFromTmp.resolve('missing-fixture', { paths: [bad, null] }));
  const ancestors = requireFromTmp.resolve.paths('fixture');
  assert.strictEqual(ancestors[0], path.join(tmp, 'node_modules'));
  assert.strictEqual(ancestors.includes(path.join(tmp, 'node_modules', 'node_modules')), false);
  console.log('require cache cleanup regressions passed');
} finally {
  Module._cache = originalCache;
  for (const key of Object.keys(originalCache)) if (key.startsWith(tmp + path.sep)) delete originalCache[key];
  delete globalThis[counter];
  fs.rmSync(tmp, { recursive: true, force: true });
}
