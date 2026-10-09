const assert = require('node:assert');
const { isBuiltin, builtinModules } = require('node:module');

assert.strictEqual(typeof process.getBuiltinModule, 'function');

for (const id of ['fs', 'node:fs', 'path', 'node:path', 'http', 'node:http', 'module', 'events']) {
  const mod = process.getBuiltinModule(id);
  assert.notStrictEqual(mod, undefined, id);
  assert.strictEqual(mod, require(id), id);
  assert.strictEqual(isBuiltin(id), true, id);
}

for (const name of ['fs', 'path', 'path/posix', 'http', 'https', 'querystring', 'events', 'module']) {
  assert.ok(builtinModules.includes(name), name);
}
assert.strictEqual(new Set(builtinModules).size, builtinModules.length);
for (const name of builtinModules) {
  assert.ok(!name.includes(':'), name);
  assert.strictEqual(isBuiltin(name), true, name);
  assert.notStrictEqual(process.getBuiltinModule(name), undefined, name);
}

assert.strictEqual(process.getBuiltinModule('node:fs'), process.getBuiltinModule('fs'));
assert.strictEqual(typeof process.getBuiltinModule('fs').readFileSync, 'function');
assert.strictEqual(typeof process.getBuiltinModule('path').join, 'function');

for (const id of ['', 'not-a-builtin', 'node:not-a-builtin', './fs', 'ant:internal/primordials', 'ant:path']) {
  assert.strictEqual(process.getBuiltinModule(id), undefined, id);
  assert.strictEqual(isBuiltin(id), false, id);
}

const invalid = [
  [undefined, 'Received undefined'],
  [null, 'Received null'],
  [1, 'Received type number (1)'],
  [{}, 'Received an instance of Object'],
  [function load() {}, 'Received function load'],
];
for (const [id, received] of invalid) {
  assert.throws(() => process.getBuiltinModule(id), {
    name: 'TypeError',
    code: 'ERR_INVALID_ARG_TYPE',
    message: `The "id" argument must be of type string. ${received}`,
  });
}
assert.throws(() => process.getBuiltinModule(Symbol('fs')), { name: 'TypeError', code: 'ERR_INVALID_ARG_TYPE' });

for (const id of ['fs', 'node:fs', 'path', 'node:path', 'http', 'util/types']) {
  assert.strictEqual(require.resolve(id), id, id);
  assert.strictEqual(require.resolve.paths(id), null, id);
}

// a NUL inside the name is not a prefix match
for (const id of ['fs\0x', 'node:fs\0', 'path\0', 'util/types\0']) {
  assert.strictEqual(isBuiltin(id), false, JSON.stringify(id));
  assert.strictEqual(process.getBuiltinModule(id), undefined, JSON.stringify(id));
}

console.log('process:get-builtin-module:ok');
