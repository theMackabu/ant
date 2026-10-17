const assert = require('node:assert');
const { inspect } = require('node:util');

function sparse(build) {
  const a = [];
  build(a);
  return a;
}

const cases = [
  [[1, , 2], '[ 1, <1 empty item>, 2 ]'],
  [[, ], '[ <1 empty item> ]'],
  [[, , 1, , , ], '[ <2 empty items>, 1, <2 empty items> ]'],
  [new Array(5), '[ <5 empty items> ]'],
  [sparse((a) => { a[3] = 1; }), '[ <3 empty items>, 1 ]'],
  [sparse((a) => { a.push(1, 2, 3); delete a[1]; }), '[ 1, <1 empty item>, 3 ]'],
  [sparse((a) => { a.push(1); a.length = 4; }), '[ 1, <3 empty items> ]'],
  [sparse((a) => { a[100] = 1; }), '[ <100 empty items>, 1 ]'],
  [sparse((a) => { a.push(1); a[2] = 3; a.foo = 'x'; }), "[ 1, <1 empty item>, 3, foo: 'x' ]"],
  [sparse((a) => { a[2] = 'b'; a[0] = 'a'; }), "[ 'a', <1 empty item>, 'b' ]"],
  [sparse((a) => { a[1e6] = 1; }), '[ <1000000 empty items>, 1 ]'],
  [sparse((a) => { a.length = 2 ** 32 - 1; a[5] = 'x'; }), "[ <5 empty items>, 'x', <4294967289 empty items> ]"],
  [Object.freeze([1, , 3]), '[ 1, <1 empty item>, 3 ]'],
  [[undefined, , null], '[ undefined, <1 empty item>, null ]'],
];

for (const [value, expected] of cases) assert.strictEqual(inspect(value), expected);

Array.prototype[1] = 'proto';
assert.strictEqual(inspect([0, , 2]), '[ 0, <1 empty item>, 2 ]');
delete Array.prototype[1];

const getter = [1, , 3];
Object.defineProperty(getter, 1, { get() { return 'got'; }, enumerable: true });
assert.strictEqual(inspect(getter), '[ 1, [Getter], 3 ]');

class Sub extends Array {}
const sub = Sub.from([1, 2]);
delete sub[0];
assert.strictEqual(inspect(sub), 'Sub(2) [ <1 empty item>, 2 ]');

assert.match(inspect([[, 'x'], { k: [, 1] }]), /\[ <1 empty item>, 'x' \][\s\S]*\[ <1 empty item>, 1 \]/);
(function () { assert.strictEqual(inspect(arguments), "[Arguments] { '0': 1, '1': 2 }"); })(1, 2);

console.log('PASS inspect array holes');
