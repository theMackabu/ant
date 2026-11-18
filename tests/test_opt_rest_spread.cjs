const assert = require('node:assert');

const WARM = 3000;
const log = [];
function take() { return log.splice(0).join(','); }

function sum3(a, b, c) { return a + b + c; }
function count() { return arguments.length; }
function restLength(...r) { return r.length; }
function forward(...a) { return sum3(...a); }
function forwardTail(x, ...a) { return sum3(x, ...a); }
function forwardCount(...a) { return count(...a); }
function forwardNew(...a) { return new Point(...a); }
function spreadCall(arr) { return sum3(...arr); }
function spreadCount(arr) { return count(...arr); }
function spreadFixed(x, arr) { return count(x, ...arr); }
function spreadNew(arr) { return new Point(...arr); }
function spreadArrayCall(x, arr) { return [x, ...arr, id(x)]; }
function spreadMax(arr) { return Math.max(...arr); }
function spreadPush(arr) { const out = [0]; out.push(...arr); return out; }
function id(x) { return x; }

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
class RestDerived extends Base {
  constructor(...args) { super(...args); this.own = args.length; }
}
class PureRestDerived extends Base {
  constructor(...args) { super(...args); }
}
class FixedDerived extends Base {
  constructor(a, ...rest) { super(a, ...rest); }
}
class ArrayDerived extends Base {
  constructor(arr) { super(...arr); }
}
function makeRest(...a) { return new RestDerived(...a); }
function makePure(a, b) { return new PureRestDerived(a, b); }
function makeFixed(a, b) { return new FixedDerived(a, b); }
function makeArray(arr) { return new ArrayDerived(arr); }

const three = [1, 2, 3];
const two = [4, 5];

function warm() {
  for (let i = 0; i < WARM; i++) {
    assert.strictEqual(forward(1, 2, 3), 6);
    assert.strictEqual(forwardTail(1, 2, 3), 6);
    assert.strictEqual(forwardCount(1, 2), 2);
    assert.strictEqual(forwardNew(1, 2, 3).n, 3);
    assert.strictEqual(spreadCall(three), 6);
    assert.strictEqual(spreadCount(three), 3);
    assert.strictEqual(spreadFixed(i, three), 4);
    assert.strictEqual(spreadNew(three).c, 3);
    assert.strictEqual(spreadArrayCall(i, two)[3], i);
    assert.strictEqual(spreadMax(three), 3);
    assert.strictEqual(spreadPush(two).length, 3);
    assert.strictEqual(spreadMethod(holder, three), 16);
    assert.strictEqual(spreadSelf(holder, two), holder);
    assert.strictEqual(makeRest(1, 2).b, 2);
  }
}
warm();

function checkShapes() {
  assert.ok(Number.isNaN(forward(1, 2)));
  assert.strictEqual(forward('a', 'b', 'c', 'd'), 'abc');
  assert.strictEqual(forwardTail('a', 'b', 'c'), 'abc');
  assert.strictEqual(forwardCount(), 0);
  assert.strictEqual(forwardCount(...new Array(20).fill(1)), 20);
  assert.strictEqual(forwardNew().n, 0);
  assert.strictEqual(forwardNew(1, 2, 3, 4, 5).n, 5);

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

  assert.deepStrictEqual(spreadArrayCall('q', three), ['q', 1, 2, 3, 'q']);
  assert.deepStrictEqual(three, [1, 2, 3]);

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

  for (const make of [makeRest, makePure, makeFixed]) {
    const made = make(1, 2);
    assert.strictEqual(made.a, 1);
    assert.strictEqual(made.b, 2);
    assert.strictEqual(made.n, 2);
    assert.ok(made instanceof Base);
  }
  assert.strictEqual(makeRest(1, 2).own, 2);
  assert.strictEqual(makeRest().n, 0);
  assert.strictEqual(makeRest().own, 0);
  assert.strictEqual(makeRest(1, 2, 3, 4).n, 4);
  assert.strictEqual(makeRest(1, 2).target, RestDerived);
}
checkShapes();
warm();
checkShapes();

const restArrowForward = (...r) => count(...r);
function restTwice(...r) { return count(...r) + count(...r); }
function restAndArguments(...r) { return [arguments.length, r.length, count(...r)]; }

for (let i = 0; i < WARM; i++) {
  assert.strictEqual(restArrowForward(1, 2), 2);
  assert.strictEqual(restTwice(1, 2), 4);
  assert.deepStrictEqual(restAndArguments(1, 2, 3), [3, 3, 3]);
}
assert.strictEqual(restTwice(), 0);
assert.deepStrictEqual(restAndArguments(), [0, 0, 0]);

function order(tag, value) { log.push(tag); return value; }
function orderCall() { return order('callee', count)(order('a', 1), ...order('spread', [2, 3])); }
function orderMethod() { return order('receiver', holder).m(order('a', 1), ...order('spread', [2, 3])); }
function orderNew() { return new (order('callee', Point))(order('a', 1), ...order('spread', [2, 3])); }
function orderArray() { return [order('a', 1), ...order('spread', [2]), order('b', 3)]; }
for (let i = 0; i < WARM; i++) {
  assert.strictEqual(orderCall(), 3);
  assert.strictEqual(take(), 'callee,a,spread');
  assert.strictEqual(orderMethod(), 16);
  assert.strictEqual(take(), 'receiver,a,spread');
  assert.strictEqual(orderNew().n, 3);
  assert.strictEqual(take(), 'callee,a,spread');
  assert.deepStrictEqual(orderArray(), [1, 2, 3]);
  assert.strictEqual(take(), 'a,spread,b');
}

function noisy(values, failAt, withReturn) {
  const iterable = {
    [Symbol.iterator]() {
      log.push('open');
      let index = 0;
      const iterator = {
        next() {
          log.push('next' + index);
          if (index === failAt) throw new RangeError('stop');
          if (index < values.length) return { value: values[index++], done: false };
          return { value: undefined, done: true };
        },
      };
      if (withReturn) iterator.return = function () { log.push('return'); return {}; };
      return iterator;
    },
  };
  return iterable;
}
for (let i = 0; i < WARM / 10; i++) {
  assert.strictEqual(spreadCount(noisy([1, 2], -1, true)), 2);
  assert.strictEqual(take(), 'open,next0,next1,next2');
  assert.throws(() => spreadCount(noisy([1, 2, 3], 1, true)), RangeError);
  assert.strictEqual(take(), 'open,next0,next1');
  assert.throws(() => spreadNew(noisy([1], 0, false)), RangeError);
  assert.strictEqual(take(), 'open,next0');
  assert.throws(() => makeArray(noisy([1], 1, false)), RangeError);
  assert.strictEqual(take(), 'open,next0,next1');
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
bigCall(() => restLength(...big), 70000);
bigCall(() => forwardCount(...big), 70000);
bigCall(() => spreadNew(big).n, 70000);
bigCall(() => makeArray(big).n, 70000);
bigCall(() => new PureRestDerived(...big).n, 70000);

function deoptInside(v) { return v + 1; }
function deoptForward(...a) { return deoptInside(...a); }
function deoptForwardCaller(v) { return deoptForward(v, 2, 3); }
for (let i = 0; i < WARM; i++) {
  assert.strictEqual(deoptForwardCaller(i), i + 1);
}
assert.strictEqual(deoptForwardCaller('s'), 's1');
for (let i = 0; i < WARM; i++) {
  assert.strictEqual(deoptForwardCaller(i), i + 1);
}

assert.ok(Reflect.construct(spreadNew.bind(null, three), []) instanceof Point);

function guardCall(arr) { return sum3(...arr); }
function guardNew(arr) { return new Point(...arr); }
function guardMethod(arr) { return holder.m(...arr); }
function guardSuper(arr) { return new ArrayDerived(arr); }
function guardRestUse(...r) { return r.length + count(...r); }
function guardRestStore(...r) { const kept = r; return count(...r) + kept.length; }
function guardRestReturn(flag, ...r) { return flag ? r : count(...r); }
for (let i = 0; i < WARM; i++) {
  assert.strictEqual(guardCall(three), 6);
  assert.strictEqual(guardNew(three).c, 3);
  assert.strictEqual(guardMethod(three), 16);
  assert.strictEqual(guardSuper(two).b, 5);
  assert.strictEqual(guardRestUse(1, 2), 4);
  assert.strictEqual(guardRestStore(1, 2), 4);
  assert.strictEqual(guardRestReturn(false, 1, 2), 2);
}
assert.deepStrictEqual(guardRestReturn(true, 1, 2), [1, 2]);
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
assert.strictEqual(forward(1, 2, 3), 6);
assert.strictEqual(take(), 'step,step,step,step');
assert.strictEqual(forwardCount(), 0);
assert.strictEqual(take(), 'step');
assert.strictEqual(makeRest(1, 2).b, 2);
assert.strictEqual(take(), 'step,step,step,step,step,step');
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
assert.strictEqual(forward(1, 2, 3), 'pqundefined');
assert.strictEqual(forwardCount(), 2);
assert.strictEqual(forwardTail(1, 2, 3), '1pq');
assert.strictEqual(makeRest(1, 2, 3).n, 2);
assert.strictEqual(makeRest(1, 2, 3).a, 'p');
assert.strictEqual(makeRest(1, 2, 3).own, 2);
assert.strictEqual(spreadNew(three).n, 2);
assert.strictEqual(spreadMethod(holder, three), '10pq0');
assert.ok(Number.isNaN(spreadMax(three)));
assert.strictEqual(restTwice(1), 4);
Array.prototype[Symbol.iterator] = arrayIterator;
assert.ok(take().length > 0);
assert.strictEqual(spreadCall(three), 6);
assert.strictEqual(forward(1, 2, 3), 6);
assert.strictEqual(makeRest(1, 2).b, 2);
assert.strictEqual(take(), '');
warm();
checkShapes();

console.log('ok');
