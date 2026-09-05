// Import for the first time only after user-visible builtins have been changed.
const saved = {
  slice: String.prototype.slice,
  charCodeAt: String.prototype.charCodeAt,
  lower: String.prototype.toLowerCase,
  bind: Function.prototype.bind,
  call: Function.prototype.call,
  isArray: Array.isArray,
  stringify: JSON.stringify,
  String,
  TypeError,
};
let p, normalized;
try {
  const poisoned = () => { throw new Error('mutable builtin used'); };
  delete String.prototype.slice;
  String.prototype.charCodeAt = poisoned;
  String.prototype.toLowerCase = poisoned;
  Function.prototype.bind = poisoned;
  Function.prototype.call = poisoned;
  Array.isArray = poisoned;
  JSON.stringify = poisoned;
  globalThis.String = poisoned;
  globalThis.TypeError = poisoned;
  p = require('ant:internal/primordials');
  normalized = require('node:path').normalize('/a/../b');
} finally {
  globalThis.String = saved.String;
  globalThis.TypeError = saved.TypeError;
  String.prototype.slice = saved.slice;
  String.prototype.charCodeAt = saved.charCodeAt;
  String.prototype.toLowerCase = saved.lower;
  Function.prototype.bind = saved.bind;
  Function.prototype.call = saved.call;
  Array.isArray = saved.isArray;
  JSON.stringify = saved.stringify;
}
const assert = require('node:assert');
assert.strictEqual(normalized, '/b');
assert.strictEqual(Object.getPrototypeOf(p), null);
assert.strictEqual(Object.isFrozen(p), true);
assert.strictEqual(require('ant:internal/primordials'), p);
assert.strictEqual(Object.keys(p).length, 19);
assert.strictEqual(p.ArrayIsArray([]), true);
assert.strictEqual(p.JSONStringify({ x: 1 }), '{"x":1}');
assert.strictEqual(p.String(123), '123');
assert.strictEqual(p.TypeError, saved.TypeError);
assert.strictEqual(p.StringPrototypeSlice('abcd', 1, 3), 'bc');
assert.strictEqual(p.StringPrototypeCharCodeAt('abc', 1), 98);
assert.strictEqual(p.StringPrototypeToLowerCase('ABC'), 'abc');
assert.strictEqual(p.StringPrototypeToUpperCase('abc'), 'ABC');
assert.strictEqual(p.StringPrototypeIncludes('abc', 'b'), true);
assert.strictEqual(p.StringPrototypeIndexOf('abc', 'b'), 1);
assert.strictEqual(p.StringPrototypeLastIndexOf('aba', 'a'), 2);
assert.strictEqual(p.StringPrototypeRepeat('a', 3), 'aaa');
assert.strictEqual(p.StringPrototypeReplace('abc', 'b', 'x'), 'axc');
assert.deepStrictEqual(p.StringPrototypeSplit('a:b', ':'), ['a', 'b']);
const values = [1];
assert.strictEqual(p.ArrayPrototypePush(values, 2), 2);
assert.strictEqual(p.ArrayPrototypeIncludes(values, 2), true);
assert.strictEqual(p.ArrayPrototypeJoin(values, ':'), '1:2');
assert.deepStrictEqual(p.ArrayPrototypeSlice(values, 1), [2]);
assert.strictEqual(p.FunctionPrototypeBind(function(x) { return this.v + x; }, { v: 2 }, 3)(), 5);
assert.strictEqual(Object.getOwnPropertyDescriptor(p, 'StringPrototypeSlice').writable, false);
assert.strictEqual(typeof primordials, 'undefined');
assert.strictEqual(arguments.length, 5);
assert.strictEqual(Object.hasOwn(Ant, '__primordials'), false);
assert.throws(() => require('ant:intrinsics'));
console.log('private primordial capture, poisoning, wrappers and module loading ok');
