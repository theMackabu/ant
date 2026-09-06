const assert = require('node:assert');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const Module = require('node:module');
const tmp = fs.realpathSync(fs.mkdtempSync(path.join(os.tmpdir(), 'ant-cache-parity-')));
const req = Module.createRequire(path.join(tmp, 'entry.cjs'));
const write = (name, source) => {
  const filename = path.join(tmp, name);
  fs.writeFileSync(filename, source);
  return filename;
};
try {
  const constructedParent = new Module('/tmp/parent.cjs');
  const constructedChild = new Module('/tmp/child.cjs', constructedParent);
  assert(constructedChild instanceof Module);
  assert.strictEqual(constructedChild.id, '/tmp/child.cjs');
  assert.strictEqual(constructedChild.filename, null);
  assert.strictEqual(constructedChild.loaded, false);
  assert.strictEqual(constructedChild.parent, constructedParent);
  assert.strictEqual(constructedParent.children[0], constructedChild);
  assert.deepStrictEqual(constructedChild.exports, {});
  assert.strictEqual(typeof constructedChild.require, 'function');
  assert.strictEqual(Module, Module.Module);
  assert.strictEqual(Module._cache, require.cache);
  assert(module instanceof Module);
  assert.strictEqual(require.cache[__filename], module);
  assert.strictEqual(require.main, module);
  assert.strictEqual(module.id, '.');
  assert.strictEqual(module.loaded, false);
  assert(Array.isArray(module.children));
  assert(Array.isArray(module.paths));
  assert.strictEqual(module.path, __dirname);
  assert.strictEqual(module.require('node:fs'), fs);

  assert.strictEqual(Object.keys(require.cache).some(key => key.startsWith('node:') || key.startsWith('ant:')), false);
  assert.strictEqual(Object.getPrototypeOf(module.exports), Object.prototype);
  const fake = {};
  require.cache.fs = { exports: fake };
  assert.strictEqual(require('fs'), fake);
  assert.strictEqual(req('fs'), fake);
  assert.strictEqual(require('node:fs'), fs);
  delete require.cache.fs;
  const file = write('value.cjs', 'module.exports = undefined;');
  assert.strictEqual(req(file), undefined);
  assert.strictEqual(req(file), undefined);
  req.cache[file] = { exports: undefined };
  assert.strictEqual(req(file), undefined);
  req.cache[file] = { exports: null };
  assert.strictEqual(req(file), null);
  req.cache[file] = Object.create({ exports: 99 });
  assert.strictEqual(req(file), 99);
  req.cache[file] = null;
  assert.throws(() => req(file), TypeError);
  delete req.cache[file];

  const parent = write('parent.cjs', 'exports.module = module; exports.child = require("./child.cjs"); require("./child.cjs");');
  const child = write('child.cjs', 'module.exports = module;');
  const p = req(parent);
  assert(p.module instanceof Module);
  assert.strictEqual(p.module.children.length, 1);
  assert.strictEqual(p.module.children[0], p.child);
  assert.strictEqual(p.child.parent, p.module);
  assert.strictEqual(p.child.require('./parent.cjs'), p);
  assert.deepStrictEqual(p.child.paths, Module.createRequire(child).resolve.paths('not-a-builtin').slice(0, p.child.paths.length));

  const failParent = write('fail-parent.cjs', 'try { require("./fail.cjs"); } catch {} module.exports = module;');
  write('fail.cjs', 'throw new Error("fail");');
  assert.strictEqual(req(failParent).children.length, 0);

  const native = write('addon.node', '');
  const originalDlopen = process.dlopen;
  let nativeLoads = 0;
  try {
    process.dlopen = (entry, filename) => {
      assert.strictEqual(filename, native);
      assert.strictEqual(Module._cache[filename], entry);
      assert.strictEqual(entry.loaded, false);
      assert(entry instanceof Module);
      entry.exports = { load: ++nativeLoads };
    };
    const firstNative = req(native);
    assert.strictEqual(req(native), firstNative);
    assert.strictEqual(req.cache[native].loaded, true);
    delete req.cache[native];
    assert.strictEqual(req(native).load, 2);
  } finally { process.dlopen = originalDlopen; }

  const replacement = Object.create(null);
  replacement[file] = { exports: 123 };
  const saved = Module._cache;
  try {
    Module._cache = replacement;
    assert.strictEqual(req(file), 123);
    assert.strictEqual(req.cache, saved);
    assert.strictEqual(Module.createRequire(file).cache, replacement);
    Module._cache = Object.create(replacement);
    assert.strictEqual(req(file), 123);
    Module._cache = null;
    assert.throws(() => req(file), TypeError);
  } finally { Module._cache = saved; }

  // Reassigning one require.cache property does not replace the loader cache.
  const old = req.cache;
  req.cache = {};
  assert.strictEqual(Module._cache, old);
  req.cache = old;
  console.log('require.cache parity tests passed');
} finally {
  delete require.cache.fs;
  for (const key of Object.keys(Module._cache)) if (key.startsWith(tmp + path.sep)) delete Module._cache[key];
  fs.rmSync(tmp, { recursive: true, force: true });
}
