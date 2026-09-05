const assert = require('node:assert');

assert.strictEqual(typeof primordials, 'undefined');
assert.strictEqual(arguments.length, 5);
assert.strictEqual(arguments[5], undefined);
assert.strictEqual(Object.hasOwn(Ant, '__primordials'), false);
assert.strictEqual(Object.hasOwn(globalThis, 'primordials'), false);

// Load path for the first time only after the public methods are replaced.
const slice = String.prototype.slice;
const charCodeAt = String.prototype.charCodeAt;
const lower = String.prototype.toLowerCase;
const bind = Function.prototype.bind;
let path;
let normalized;
let relative;
let formatted;
try {
  const poisoned = () => { throw new Error('public prototype used'); };
  String.prototype.slice = poisoned;
  String.prototype.charCodeAt = poisoned;
  String.prototype.toLowerCase = poisoned;
  Function.prototype.bind = poisoned;
  path = require('node:path');
  normalized = path.posix.normalize('/a/../b');
  relative = path.win32.relative('C:\\A\\x', 'c:\\a\\y');
  formatted = path.posix.format({ dir: '/a', base: 'b' });
} finally {
  String.prototype.slice = slice;
  String.prototype.charCodeAt = charCodeAt;
  String.prototype.toLowerCase = lower;
  Function.prototype.bind = bind;
}
assert.strictEqual(normalized, '/b');
assert.strictEqual(relative, '..\\y');
assert.strictEqual(formatted, '/a/b');
assert.strictEqual(require('path'), path);
assert.strictEqual(require('ant:path'), path);
for (const style of ['posix', 'win32']) {
  for (const prefix of ['', 'node:', 'ant:']) {
    assert.strictEqual(require(`${prefix}path/${style}`), path[style]);
  }
}
console.log('private path primordials and aliases ok');
