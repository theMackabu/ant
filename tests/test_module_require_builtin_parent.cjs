const assert = require('node:assert');
const Module = require('node:module');
const path = require('node:path');

const { require: moduleRequire } = Module.prototype;

assert.strictEqual(typeof moduleRequire, 'function');
assert.strictEqual(moduleRequire.call(module, 'node:path'), path);
assert.strictEqual(module.require('node:path'), path);

const child = new Module('/virtual/child.js', module);
child.filename = '/virtual/child.js';
child.paths = module.paths;
assert.strictEqual(moduleRequire.call(child, 'node:path'), path);

console.log('module-require-builtin-parent:ok');
