const assert = require('node:assert');

function get(x) { return x.foo; }
function call(s, i) { return s.charCodeAt(i); }
function uncurried(f, s, i) { return f.call(s, i); }
function fixed(n) { return n.toFixed(1); }
function text(b) { return b.toString(); }

for (let i = 0; i < 3000; i++) {
  call('abc', i % 3);
  uncurried(String.prototype.charCodeAt, 'abc', i % 3);
  fixed(i);
  text(i % 2 === 0);
  get('x');
  get(1);
}

String.prototype.foo = 'S';
Number.prototype.foo = 'N';
assert.deepStrictEqual([get('x'), get(1), get(true)], ['S', 'N', undefined]);

for (let i = 0; i < 3000; i++) get('y');
delete String.prototype.foo;
Object.prototype.foo = 'O';
assert.deepStrictEqual([get('x'), get(2)], ['O', 'N']);
delete Object.prototype.foo;
delete Number.prototype.foo;

const original = String.prototype.charCodeAt;
String.prototype.charCodeAt = function (i) { return -i; };
assert.strictEqual(call('abc', 2), -2);
for (let i = 0; i < 3000; i++) call('abc', 1);
assert.strictEqual(call('abc', 2), -2);
String.prototype.charCodeAt = original;

assert.deepStrictEqual(
  [call('abc', 2), call('é', 0), call('abc', 9), call('abc', -1), call('abc', 1.7), call(new String('abc'), 0), call('abc', '1'), call('abc')],
  [99, 233, NaN, NaN, 98, 97, 98, 97]
);
assert.deepStrictEqual(
  [uncurried(original, 'abc', 0), uncurried(original, 'é', 0), uncurried(original, 'abc', 5)],
  [97, 233, NaN]
);

Object.defineProperty(Number.prototype, 'toFixed', { get() { return () => 'getter'; }, configurable: true });
assert.strictEqual(fixed(3), 'getter');

let sum = 0;
for (let i = 0; i < 100000; i++) sum += 'hello world'.length + i.toString().length + 'xyz'.charCodeAt(i % 3);
assert.strictEqual(sum, 13688889);

class EdgeHolder { m() { return 1; } }
const edgeHolder = new EdgeHolder();
const edgeOps = {
  instanceLeft: v => v instanceof EdgeHolder,
  get: v => v.x,
  call: v => { try { return v.m(); } catch (e) { return e.name; } },
  index: v => v[0],
  has: v => { try { return 'x' in v; } catch (e) { return e.name; } },
  length: v => v.length,
  proto: v => v.__proto__ === Number.prototype,
  ctor: v => v.constructor === Number,
  text: v => v.toString(),
  pattern: v => { const { x } = v; return x; },
  optional: v => v?.x,
  own: v => Object.hasOwn(v, 'x'),
};
const edgeWarm = [edgeHolder, [1, 2], { x: 1, m() { return 2; }, 0: 5, length: 1 }, function () { return 3; }, 'str', EdgeHolder];
const edgeValues = [-Infinity, Infinity, NaN, -0, 1.5, -1e308, -1.7976931348623157e308];
const edgeExpected = {
  instanceLeft: v => false,
  get: v => undefined,
  call: v => 'TypeError',
  index: v => undefined,
  has: v => 'TypeError',
  length: v => undefined,
  proto: v => true,
  ctor: v => true,
  text: v => String(v),
  pattern: v => undefined,
  optional: v => undefined,
  own: v => false,
};
for (const [name, op] of Object.entries(edgeOps)) {
  for (let i = 0; i < 400; i++) { try { op(edgeWarm[i % edgeWarm.length]); } catch (e) {} }
  for (const v of edgeValues) assert.strictEqual(op(v), edgeExpected[name](v), `${name} on ${v}`);
}

console.log('jit primitive field ic OK');
