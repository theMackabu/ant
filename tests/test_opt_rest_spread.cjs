const assert = require('node:assert');

const WARM = 3000;
const log = [];
function take() { return log.splice(0).join(','); }

function sum3(a, b, c) { return a + b + c; }
function count() { return arguments.length; }
function spreadCall(arr) { return sum3(...arr); }
function spreadCount(arr) { return count(...arr); }
function spreadFixed(x, arr) { return count(x, ...arr); }
function spreadNew(arr) { return new Point(...arr); }
function spreadMax(arr) { return Math.max(...arr); }
function spreadPush(arr) { const out = [0]; out.push(...arr); return out; }

function Point(a, b, c) {
  this.a = a;
  this.b = b;
  this.c = c;
  this.n = arguments.length;
  this.target = new.target;
}

const holder = {
  k: 10,
  m(a, b, c) { return this.k + a + b + (c === undefined ? 0 : c); },
  self() { return this; },
};
function spreadMethod(o, arr) { return o.m(...arr); }
function spreadSelf(o, arr) { return o.self(...arr); }

class Base {
  constructor(a, b) {
    this.a = a;
    this.b = b;
    this.n = arguments.length;
    this.target = new.target;
  }
}
class PureRestDerived extends Base {
  constructor(...args) { super(...args); }
}
class ArrayDerived extends Base {
  constructor(arr) { super(...arr); }
}
function makeArray(arr) { return new ArrayDerived(arr); }

const three = [1, 2, 3];
const two = [4, 5];

function warm() {
  for (let i = 0; i < WARM; i++) {
    assert.strictEqual(spreadCall(three), 6);
    assert.strictEqual(spreadCount(three), 3);
    assert.strictEqual(spreadFixed(i, three), 4);
    assert.strictEqual(spreadNew(three).c, 3);
    assert.strictEqual(spreadMax(three), 3);
    assert.strictEqual(spreadPush(two).length, 3);
    assert.strictEqual(spreadMethod(holder, three), 16);
    assert.strictEqual(spreadSelf(holder, two), holder);
  }
}
warm();

function checkShapes() {
  assert.strictEqual(spreadCall(three), 6);
  assert.strictEqual(spreadCall(['x', 'y', 'z']), 'xyz');
  assert.ok(Number.isNaN(spreadCall(two)));
  assert.ok(Number.isNaN(spreadCall([])));
  assert.strictEqual(spreadCall([1, 2, 3, 4]), 6);
  assert.strictEqual(spreadCount([]), 0);
  assert.strictEqual(spreadCount(two), 2);
  assert.strictEqual(spreadCount(new Array(17).fill(1)), 17);
  assert.strictEqual(spreadCount(new Array(70).fill(1)), 70);
  assert.strictEqual(spreadCount([1, , 3]), 3);
  assert.ok(Number.isNaN(spreadCall([1, , 3])));
  assert.strictEqual(spreadCount('héllo'), 5);
  assert.strictEqual(spreadCount(new Set([1, 2, 2, 3])), 3);
  assert.strictEqual(spreadCount(new Map([[1, 2], [3, 4]])), 2);
  assert.strictEqual(spreadCount(new Uint8Array([1, 2, 3, 4])), 4);
  assert.strictEqual(spreadCount((function* () { yield 1; yield 2; })()), 2);
  assert.strictEqual((function () { return spreadCount(arguments); })(1, 2, 3, 4), 4);
  assert.strictEqual((function () { return spreadCall(arguments); })(1, 2, 3), 6);
  assert.strictEqual(spreadFixed(0, []), 1);
  assert.strictEqual(spreadFixed(0, new Array(16).fill(1)), 17);
  assert.throws(() => spreadCall(5), TypeError);
  assert.throws(() => spreadCall(null), TypeError);
  assert.throws(() => spreadCall(undefined), TypeError);
  assert.throws(() => spreadCall({}), TypeError);
  assert.throws(() => spreadNew({}), TypeError);

  assert.strictEqual(spreadNew(three).n, 3);
  assert.strictEqual(spreadNew(three).target, Point);
  assert.strictEqual(spreadNew([]).n, 0);
  assert.strictEqual(spreadNew(new Set([1, 2])).b, 2);
  assert.strictEqual(spreadNew(new Array(20).fill(3)).n, 20);

  assert.strictEqual(spreadMax(three), 3);
  assert.strictEqual(spreadMax([]), -Infinity);
  assert.strictEqual(spreadMax([5]), 5);
  assert.strictEqual(spreadMax([1, 9, 2, 8]), 9);
  assert.ok(Number.isNaN(spreadMax([1, 'x', 3])));
  assert.ok(Number.isNaN(spreadMax([1, , 3])));
  assert.strictEqual(spreadMax([1, '7', 3]), 7);
  assert.deepStrictEqual(spreadPush(three), [0, 1, 2, 3]);
  assert.deepStrictEqual(spreadPush([]), [0]);

  assert.strictEqual(spreadMethod(holder, three), 16);
  assert.strictEqual(spreadMethod(holder, two), 19);
  assert.strictEqual(spreadMethod({ k: 1, m: holder.m }, three), 7);
  assert.strictEqual(spreadMethod({ m() { return this; } }, []).constructor, Object);
  const other = { self: holder.self };
  assert.strictEqual(spreadSelf(other, three), other);
  assert.throws(() => spreadMethod({}, three), TypeError);
}
checkShapes();
warm();
checkShapes();

function order(tag, value) { log.push(tag); return value; }
function orderCall() { return order('callee', count)(order('a', 1), ...order('spread', [2, 3])); }
function orderMethod() { return order('receiver', holder).m(order('a', 1), ...order('spread', [2, 3])); }
function orderNew() { return new (order('callee', Point))(order('a', 1), ...order('spread', [2, 3])); }
for (let i = 0; i < WARM; i++) {
  assert.strictEqual(orderCall(), 3);
  assert.strictEqual(take(), 'callee,a,spread');
  assert.strictEqual(orderMethod(), 16);
  assert.strictEqual(take(), 'receiver,a,spread');
  assert.strictEqual(orderNew().n, 3);
  assert.strictEqual(take(), 'callee,a,spread');
}

const big = Array.from({ length: 70000 }, (_, i) => i);
function bigCall(run, expected) {
  try {
    assert.strictEqual(run(), expected);
  } catch (error) {
    assert.ok(error instanceof RangeError);
  }
}
bigCall(() => spreadMax(big), 69999);
bigCall(() => spreadCount(big), 70000);
bigCall(() => spreadNew(big).n, 70000);
bigCall(() => makeArray(big).n, 70000);
bigCall(() => new PureRestDerived(...big).n, 70000);

assert.ok(Reflect.construct(spreadNew.bind(null, three), []) instanceof Point);

function guardCall(arr) { return sum3(...arr); }
function guardNew(arr) { return new Point(...arr); }
function guardMethod(arr) { return holder.m(...arr); }
function guardSuper(arr) { return new ArrayDerived(arr); }
for (let i = 0; i < WARM; i++) {
  assert.strictEqual(guardCall(three), 6);
  assert.strictEqual(guardNew(three).c, 3);
  assert.strictEqual(guardMethod(three), 16);
  assert.strictEqual(guardSuper(two).b, 5);
}
class Quiet extends Array {
  *[Symbol.iterator]() { yield 'a'; yield 'b'; yield 'c'; yield 'd'; }
}
const quiet3 = new Quiet(1, 2, 3);
const quiet2 = new Quiet(4, 5);
assert.strictEqual(quiet3.length, 3);
assert.strictEqual(guardCall(quiet3), 'abc');
assert.strictEqual(guardNew(quiet3).n, 4);
assert.strictEqual(guardMethod(quiet3), '10abc');
assert.strictEqual(guardSuper(quiet2).n, 4);
for (let i = 0; i < WARM; i++) {
  assert.strictEqual(guardCall(three), 6);
  assert.strictEqual(guardNew(three).c, 3);
}
const sparse3 = [1, , 3];
assert.ok(Number.isNaN(guardCall(sparse3)));
assert.strictEqual(guardNew(sparse3).b, undefined);
assert.strictEqual(guardNew(sparse3).n, 3);

const arrayIterator = Array.prototype[Symbol.iterator];
const arrayIteratorProto = Object.getPrototypeOf([][Symbol.iterator]());
const arrayIteratorNext = arrayIteratorProto.next;

arrayIteratorProto.next = function () {
  log.push('step');
  return arrayIteratorNext.call(this);
};
assert.strictEqual(spreadCall(three), 6);
assert.strictEqual(take(), 'step,step,step,step');
assert.strictEqual(spreadNew(three).n, 3);
assert.strictEqual(take(), 'step,step,step,step');
assert.strictEqual(spreadMethod(holder, three), 16);
assert.strictEqual(take(), 'step,step,step,step');
assert.strictEqual(spreadMax(three), 3);
assert.strictEqual(take(), 'step,step,step,step');
arrayIteratorProto.next = arrayIteratorNext;
assert.strictEqual(spreadCall(three), 6);
assert.strictEqual(take(), '');

Array.prototype[Symbol.iterator] = function* () {
  log.push('iterate');
  yield 'p';
  yield 'q';
};
assert.strictEqual(spreadCall(three), 'pqundefined');
assert.strictEqual(spreadCount(new Array(9)), 2);
assert.strictEqual(spreadNew(three).n, 2);
assert.strictEqual(spreadMethod(holder, three), '10pq0');
assert.ok(Number.isNaN(spreadMax(three)));
Array.prototype[Symbol.iterator] = arrayIterator;
assert.ok(take().length > 0);
assert.strictEqual(spreadCall(three), 6);
warm();
checkShapes();

console.log('ok');
