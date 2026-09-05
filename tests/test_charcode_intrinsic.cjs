const assert = require('node:assert');
const original = String.prototype.charCodeAt;
const captured = Function.prototype.call.bind(original);

function makePaired(source) {
  const { StringPrototypeCharCodeAt: read } = source;
  return function paired(s, i) { return read(s, i) + read(s, i); };
}
const paired = makePaired({StringPrototypeCharCodeAt: captured});
const pairedReplacement = makePaired({StringPrototypeCharCodeAt: () => 7});
const pairedSlice = makePaired({StringPrototypeCharCodeAt: Function.prototype.call.bind(String.prototype.slice)});
for (let i = 0; i < 30000; i++) {
  assert.strictEqual(paired('abc', 1), 196);
  assert.strictEqual(pairedReplacement('abc', 1), 14);
  assert.strictEqual(pairedSlice('abc', 1), 'bcbc');
}
assert.strictEqual(paired('é😀', 1), 0xd83d * 2);
assert.ok(Number.isNaN(paired('abc', Infinity)));

function makeExtra(source) {
  const { StringPrototypeCharCodeAt: read } = source;
  return function extra(s, i, effect) { return read(s, i, effect()); };
}
const extra = makeExtra({StringPrototypeCharCodeAt: captured});
const extraFallback = makeExtra({StringPrototypeCharCodeAt: (s, i, value) => s + i + value});
let extraCalls = 0;
function extraEffect() { extraCalls++; return '!'; }
for (let i = 0; i < 1000; i++) assert.strictEqual(extra('abc', 1, extraEffect), 98);
assert.strictEqual(extraFallback('abc', 1, extraEffect), 'abc1!');
assert.strictEqual(extraCalls, 1001);
const extraError = new Error('extra argument');
assert.throws(() => extra('abc', 1, () => { throw extraError; }), e => e === extraError);

function direct(s, i) { const value = s.charCodeAt(i); return value; }
function aliases(source) {
  let { StringPrototypeCharCodeAt: read } = source;
  function scan(s, i) { const value = read(s, i); return value; }
  for (let i = 0; i < 30000; i++) assert.strictEqual(scan('abc', i % 3), 97 + i % 3);
  check(scan);
  read = (s, i) => s.length + i;
  assert.strictEqual(scan('abc', 1), 4);
  function shadow(read) { const value = read('abc', 1); return value; }
  assert.strictEqual(shadow(() => 123), 123);
}
function check(fn) {
  for (const [i, expected] of [[0, 97], [-0.5, 97], [1.9, 98], [NaN, 97], [undefined, 97], ['1', 98]])
    assert.strictEqual(fn('abc', i), expected);
  for (const i of [-1, 3, Infinity, -Infinity, 1e100, -1e100])
    assert.ok(Number.isNaN(fn('abc', i)));
  assert.ok(Number.isNaN(fn('', 0)));
  assert.strictEqual(fn('é😀', 0), 233);
  assert.strictEqual(fn('é😀', 1), 0xd83d);
  assert.strictEqual(fn('é😀', 2), 0xde00);
  assert.strictEqual(fn('x'.repeat(4096) + 'z', 4096), 122);
  // Exercise first reads and cached flat storage without assuming rope layout.
  for (const suffix of ['z', 'é', '😀']) {
    const rope = 'x'.repeat(4096) + suffix;
    const expected = suffix === 'z' ? 122 : suffix === 'é' ? 233 : 0xd83d;
    for (let i = 0; i < 1000; i++) {
      assert.strictEqual(fn(rope, 4096), expected);
      assert.strictEqual(fn(rope, 4095), 120);
      assert.ok(Number.isNaN(fn(rope, rope.length)));
    }
    if (suffix === '😀') assert.strictEqual(fn(rope, 4097), 0xde00);
  }
  const error = new Error('index conversion');
  assert.throws(() => fn('abc', { valueOf() { throw error; } }), e => e === error);
}
for (let i = 0; i < 30000; i++) assert.strictEqual(direct('abc', i % 3), 97 + i % 3);
check(direct);
aliases({ StringPrototypeCharCodeAt: captured });
function uncurriedVariants(source) {
  const { StringPrototypeCharCodeAt: read } = source;
  function scan(s, i) { const result = read(s, i); return result; }
  for (let i = 0; i < 30000; i++) assert.strictEqual(scan('abc', 1), 98);
  check(scan);
}
uncurriedVariants({ StringPrototypeCharCodeAt: captured.bind(null) });
function mismatchedNative() {
  const { StringPrototypeCharCodeAt: read } = {
    StringPrototypeCharCodeAt: Function.prototype.call.bind(String.prototype.slice)
  };
  for (let i = 0; i < 30000; i++) assert.strictEqual(read('abc', 1), 'bc');
}
mismatchedNative();
function preappliedUncurried() {
  const { StringPrototypeCharCodeAt: read } = {
    StringPrototypeCharCodeAt: captured.bind(null, 'abc')
  };
  for (let i = 0; i < 30000; i++) assert.strictEqual(read(1, 0), 98);
}
preappliedUncurried();

let effects = 0;
try {
  String.prototype.charCodeAt = i => { effects++; return 1000 + i; };
  assert.strictEqual(direct('abc', 2), 1002);
  assert.strictEqual(captured('abc', 2), 99);
  assert.strictEqual(effects, 1);
} finally { String.prototype.charCodeAt = original; }
const receiver = {
  get charCodeAt() { effects++; return original; },
  toString() { return 'xyz'; }
};
assert.strictEqual(direct(receiver, 1), 121);
assert.strictEqual(effects, 2);
const bound = { charCodeAt: original.bind('abc') };
const prebound = { charCodeAt: original.bind('abc', 1) };
for (let i = 0; i < 30000; i++) {
  assert.strictEqual(direct(bound, 2), 99);
  assert.strictEqual(direct(prebound, 2), 98);
}
const proxy = new Proxy(original.bind('abc'), { apply(target, self, args) { return 1234; } });
assert.strictEqual(direct({ charCodeAt: proxy }, 0), 1234);
function absent() { const result = 'abc'.charCodeAt(); return result; }
for (let i = 0; i < 30000; i++) assert.strictEqual(absent(), 97);
assert.strictEqual(null?.charCodeAt(0), undefined);
assert.strictEqual('abc'.charCodeAt?.(1), 98);
assert.strictEqual('abc'.charCodeAt(...[2]), 99);
const sentinel = new Error('receiver conversion');
assert.throws(() => direct({ charCodeAt: original, toString() { throw sentinel; } }, 0), e => e === sentinel);
console.log('charCodeAt intrinsic: direct, aliases, guards and fallbacks ok');
