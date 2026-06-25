const assert = require('node:assert');
const vm = require('node:vm');

assert.strictEqual(typeof vm.Script, 'function');
assert.strictEqual(typeof vm.runInThisContext, 'function');
assert.strictEqual(typeof vm.runInNewContext, 'function');
assert.strictEqual(typeof vm.runInContext, 'function');
assert.strictEqual(typeof vm.compileFunction, 'function');
assert.strictEqual(typeof vm.measureMemory, 'function');

assert.strictEqual(vm.runInThisContext('1 + 2'), 3);

const script = new vm.Script('(function (value) { return value + 4; })', {
  filename: 'fixture.vm.js'
});
assert.strictEqual(script.runInThisContext()(38), 42);
assert.strictEqual(script.cachedDataRejected, undefined);
assert.ok(script.createCachedData() instanceof Uint8Array);

const context = vm.createContext({ value: 41 });
assert.strictEqual(vm.isContext(context), true);
assert.strictEqual(vm.runInContext('value + 1', context), 42);

assert.strictEqual(vm.runInNewContext('value + 2', { value: 40 }), 42);

const fn = vm.compileFunction('return left + right;', ['left', 'right']);
assert.strictEqual(fn(20, 22), 42);

const { createRequire, Module } = require('node:module');
const moduleBuiltin = require('module');
assert.strictEqual(moduleBuiltin, moduleBuiltin.Module);
assert.strictEqual(moduleBuiltin.Module, Module);
assert.strictEqual(typeof moduleBuiltin.prototype.require, 'function');
assert.strictEqual(typeof moduleBuiltin.createRequire, 'function');
const requireFromHere = createRequire(__filename);
assert.strictEqual(typeof requireFromHere.cache, 'object');
assert.strictEqual(typeof requireFromHere.extensions, 'object');
assert.strictEqual(typeof Module, 'function');
assert.strictEqual(Module.prototype.constructor, Module);
assert.strictEqual(typeof Module.prototype.require, 'function');
assert.ok(Array.isArray(Module._nodeModulePaths(__dirname)));

const synthetic = new Module('/tmp/ant-vm-test/example.js');
assert.ok(synthetic instanceof Module);
assert.strictEqual(synthetic.constructor, Module);
assert.strictEqual(synthetic.id, '/tmp/ant-vm-test/example.js');
assert.strictEqual(synthetic.loaded, false);
assert.strictEqual(typeof synthetic.exports, 'object');
assert.ok(Array.isArray(synthetic.children));
assert.ok(Array.isArray(synthetic.paths));
