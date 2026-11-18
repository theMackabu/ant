const assert = require('node:assert');

const WARM = 3000;
const log = [];
function take() { return log.splice(0).join(','); }

function count() { return arguments.length; }
function list(...r) { return r; }
function first(a, ...r) { return [a, r]; }
function restLength(...r) { return r.length; }
function restIndex(a, ...r) { return r[0] + r[1] + a; }

function warm() {
  for (let i = 0; i < WARM; i++) {
    assert.strictEqual(restLength(1, 2, 3), 3);
    assert.strictEqual(restIndex(1, 2, 3), 6);
  }
}
warm();

function checkShapes() {
  assert.deepStrictEqual(list(), []);
  assert.deepStrictEqual(list(1), [1]);
  assert.deepStrictEqual(first(), [undefined, []]);
  assert.deepStrictEqual(first(1), [1, []]);
  assert.deepStrictEqual(first(1, 2, 3), [1, [2, 3]]);
  assert.strictEqual(restLength(), 0);
  assert.strictEqual(restLength(...new Array(40).fill(0)), 40);
  assert.ok(Number.isNaN(restIndex(1)));
}
checkShapes();
warm();
checkShapes();

function restFresh(...r) { return r; }
function restMutate(a, ...r) {
  r[0] = 'changed';
  r.push('more');
  return [a, arguments[1], arguments.length, r];
}
function restDefaults(a = log.push('a'), b = log.push('b'), ...r) { return [a, b, r]; }
function restDestructure({ x }, [y] = [log.push('y')], ...r) { return [x, y, r]; }
function restPattern(a, ...[b, c = 'c', ...d]) { return [a, b, c, d]; }
const restArrow = (...r) => r.length;
function restEscape(...r) { return () => r; }
function restLoop(...r) {
  let total = 0;
  for (let i = 0; i < r.length; i++) total += r[i];
  return total;
}

for (let i = 0; i < WARM; i++) {
  const a = restFresh(1, 2), b = restFresh(1, 2);
  assert.notStrictEqual(a, b);
  assert.ok(Array.isArray(a));
  assert.strictEqual(Object.getPrototypeOf(a), Array.prototype);
  assert.deepStrictEqual(restMutate(1, 2, 3), [1, 2, 3, ['changed', 3, 'more']]);
  assert.deepStrictEqual(restDefaults(1, 2, 3), [1, 2, [3]]);
  assert.deepStrictEqual(restPattern(1, 2), [1, 2, 'c', []]);
  assert.strictEqual(restArrow(1, 2, 3), 3);
  assert.strictEqual(restLoop(1, 2, 3, 4), 10);
}
assert.deepStrictEqual(restDefaults(), [1, 2, []]);
assert.strictEqual(take(), 'a,b');
assert.deepStrictEqual(restDefaults(undefined, 5, 6, 7), [1, 5, [6, 7]]);
assert.strictEqual(take(), 'a');
assert.deepStrictEqual(restDestructure({ x: 1 }), [1, 1, []]);
assert.strictEqual(take(), 'y');
assert.deepStrictEqual(restDestructure({ x: 1 }, [2], 3, 4), [1, 2, [3, 4]]);
assert.throws(() => restDestructure(), TypeError);
assert.deepStrictEqual(restPattern(1, 2, 3, 4, 5), [1, 2, 3, [4, 5]]);
assert.deepStrictEqual(restPattern(), [undefined, undefined, 'c', []]);
assert.deepStrictEqual(restEscape(1, 2)(), [1, 2]);
assert.strictEqual(restLoop(), 0);
assert.strictEqual(restLength.length, 0);
assert.strictEqual(first.length, 1);
assert.strictEqual(restDefaults.length, 0);
assert.strictEqual(restDestructure.length, 1);
assert.strictEqual(restArrow.length, 0);

function deoptBefore(x, ...a) {
  const y = x + 1;
  return count(...a) + y;
}
function deoptCaller(x) { return deoptBefore(x, 1, 2, 3); }
for (let i = 0; i < WARM; i++) {
  assert.strictEqual(deoptBefore(i, 1, 2), i + 3);
  assert.strictEqual(deoptCaller(i), i + 4);
}
assert.strictEqual(deoptBefore('s', 1, 2), '2s1');
assert.strictEqual(deoptCaller('s'), '3s1');
assert.strictEqual(deoptBefore(1.5, 1), 3.5);
assert.strictEqual(deoptCaller(2 ** 31 - 1), 2 ** 31 + 3);
for (let i = 0; i < WARM; i++) {
  assert.strictEqual(deoptCaller(i), i + 4);
}

const arrayIterator = Array.prototype[Symbol.iterator];
const arrayIteratorProto = Object.getPrototypeOf([][Symbol.iterator]());
const arrayIteratorNext = arrayIteratorProto.next;

arrayIteratorProto.next = function () {
  log.push('step');
  return arrayIteratorNext.call(this);
};
assert.strictEqual(deoptCaller(1), 5);
assert.strictEqual(take(), 'step,step,step,step');
assert.strictEqual(restLength(1, 2, 3), 3);
assert.strictEqual(take(), '');
arrayIteratorProto.next = arrayIteratorNext;

Array.prototype[Symbol.iterator] = function* () {
  log.push('iterate');
  yield 'p';
  yield 'q';
};
assert.strictEqual(deoptCaller(1), 4);
assert.strictEqual(restLength(1, 2, 3), 3);
Array.prototype[Symbol.iterator] = arrayIterator;
assert.ok(take().length > 0);
warm();
checkShapes();

console.log('ok');
