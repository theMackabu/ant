const assert = require('node:assert');

const WARM = 3000;
const log = [];
function take() { return log.splice(0).join(','); }

function sum3(a, b, c) { return a + b + c; }
function count() { return arguments.length; }
function list(...r) { return r; }
function restLength(...r) { return r.length; }
function forward(...a) { return sum3(...a); }
function forwardTail(x, ...a) { return sum3(x, ...a); }
function forwardCount(...a) { return count(...a); }
function forwardNew(...a) { return new Point(...a); }
function spreadCall(arr) { return sum3(...arr); }
function spreadCount(arr) { return count(...arr); }
function spreadFixed(x, arr) { return count(x, ...arr); }
function spreadMixed(x, arr, y) { return list(x, ...arr, y, ...arr); }
function spreadNew(arr) { return new Point(...arr); }
function spreadNewMixed(arr, y) { return new Point(...arr, y); }
function spreadArray(arr) { return [...arr]; }
function spreadArrayPrefix(x, arr) { return [x, ...arr]; }
function spreadArrayMixed(x, arr, y) { return [x, ...arr, y, ...arr]; }
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
class MixedDerived extends Base {
  constructor(arr, y) { super(...arr, y); }
}
function makeRest(...a) { return new RestDerived(...a); }
function makePure(a, b) { return new PureRestDerived(a, b); }
function makeFixed(a, b) { return new FixedDerived(a, b); }
function makeArray(arr) { return new ArrayDerived(arr); }
function makeMixed(arr, y) { return new MixedDerived(arr, y); }

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
    assert.strictEqual(spreadMixed(0, two, 9).length, 6);
    assert.strictEqual(spreadNew(three).c, 3);
    assert.strictEqual(spreadNewMixed(two, 7).c, 7);
    assert.strictEqual(spreadArray(three).length, 3);
    assert.strictEqual(spreadArrayPrefix(i, three).length, 4);
    assert.strictEqual(spreadArrayMixed(i, two, 9).length, 6);
    assert.strictEqual(spreadArrayCall(i, two)[3], i);
    assert.strictEqual(spreadMax(three), 3);
    assert.strictEqual(spreadPush(two).length, 3);
    assert.strictEqual(spreadMethod(holder, three), 16);
    assert.strictEqual(spreadSelf(holder, two), holder);
    assert.strictEqual(makeRest(1, 2).b, 2);
    assert.strictEqual(makeMixed(two, 6).n, 3);
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
  assert.deepStrictEqual(spreadArray([1, , 3]), [1, undefined, 3]);
  assert.ok(1 in spreadArray([1, , 3]));
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
  assert.deepStrictEqual(spreadMixed('x', [1], 'y'), ['x', 1, 'y', 1]);
  assert.deepStrictEqual(spreadMixed('x', new Set([1, 2]), 'y'), ['x', 1, 2, 'y', 1, 2]);
  assert.throws(() => spreadCall(5), TypeError);
  assert.throws(() => spreadCall(null), TypeError);
  assert.throws(() => spreadCall(undefined), TypeError);
  assert.throws(() => spreadCall({}), TypeError);
  assert.throws(() => spreadArray(7), TypeError);
  assert.throws(() => spreadNew({}), TypeError);

  assert.strictEqual(spreadNew(three).n, 3);
  assert.strictEqual(spreadNew(three).target, Point);
  assert.strictEqual(spreadNew([]).n, 0);
  assert.strictEqual(spreadNew(new Set([1, 2])).b, 2);
  assert.strictEqual(spreadNew(new Array(20).fill(3)).n, 20);
  assert.strictEqual(spreadNewMixed([], 1).a, 1);
  assert.strictEqual(spreadNewMixed('ab', 1).c, 1);

  assert.deepStrictEqual(spreadArray(three), [1, 2, 3]);
  assert.notStrictEqual(spreadArray(three), three);
  assert.notStrictEqual(spreadArray(three), spreadArray(three));
  assert.deepStrictEqual(spreadArray([]), []);
  assert.deepStrictEqual(spreadArray(two), [4, 5]);
  assert.deepStrictEqual(spreadArray('ab'), ['a', 'b']);
  assert.deepStrictEqual(spreadArray(new Set([3, 1])), [3, 1]);
  assert.strictEqual(spreadArray(new Array(100).fill(2)).length, 100);
  assert.deepStrictEqual(spreadArrayPrefix(0, two), [0, 4, 5]);
  assert.deepStrictEqual(spreadArrayPrefix(0, []), [0]);
  assert.deepStrictEqual(spreadArrayPrefix(0, [, 1]), [0, undefined, 1]);
  assert.deepStrictEqual(spreadArrayMixed(0, [1], 2), [0, 1, 2, 1]);
  assert.deepStrictEqual(spreadArrayMixed(0, 'ab', 2), [0, 'a', 'b', 2, 'a', 'b']);
  assert.deepStrictEqual(spreadArrayCall('q', three), ['q', 1, 2, 3, 'q']);
  const copy = spreadArray(three);
  copy.push(4);
  copy[0] = 'changed';
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
  assert.strictEqual(makeMixed(two, 6).n, 3);
  assert.strictEqual(makeMixed([], 6).a, 6);
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
function orderMixed() { return order('callee', count)(...order('s1', [1]), order('a', 2), ...order('s2', [3])); }
for (let i = 0; i < WARM; i++) {
  assert.strictEqual(orderCall(), 3);
  assert.strictEqual(take(), 'callee,a,spread');
  assert.strictEqual(orderMethod(), 16);
  assert.strictEqual(take(), 'receiver,a,spread');
  assert.strictEqual(orderNew().n, 3);
  assert.strictEqual(take(), 'callee,a,spread');
  assert.deepStrictEqual(orderArray(), [1, 2, 3]);
  assert.strictEqual(take(), 'a,spread,b');
  assert.strictEqual(orderMixed(), 3);
  assert.strictEqual(take(), 'callee,s1,a,s2');
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
  assert.deepStrictEqual(spreadArray(noisy(['a'], -1, true)), ['a']);
  assert.strictEqual(take(), 'open,next0,next1');
  assert.throws(() => spreadCount(noisy([1, 2, 3], 1, true)), RangeError);
  assert.strictEqual(take(), 'open,next0,next1');
  assert.throws(() => spreadArrayPrefix(0, noisy([1, 2, 3], 2, true)), RangeError);
  assert.strictEqual(take(), 'open,next0,next1,next2');
  assert.throws(() => spreadNew(noisy([1], 0, false)), RangeError);
  assert.strictEqual(take(), 'open,next0');
  assert.throws(() => makeArray(noisy([1], 1, false)), RangeError);
  assert.strictEqual(take(), 'open,next0,next1');
  assert.deepStrictEqual(spreadMixed('x', noisy([1], -1, false), 'y'), ['x', 1, 'y', 1]);
  assert.strictEqual(take(), 'open,next0,next1,open,next0,next1');
}

const big = Array.from({ length: 70000 }, (_, i) => i);
function bigCall(run, expected) {
  try {
    assert.strictEqual(run(), expected);
  } catch (error) {
    assert.ok(error instanceof RangeError);
  }
}
assert.strictEqual(spreadArray(big).length, 70000);
assert.strictEqual(spreadArrayPrefix(0, big).length, 70001);
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
function guardArray(arr) { return [...arr]; }
function guardPrefix(x, arr) { return [x, ...arr]; }
function guardNew(arr) { return new Point(...arr); }
function guardMethod(arr) { return holder.m(...arr); }
function guardSuper(arr) { return new ArrayDerived(arr); }
function guardRestUse(...r) { return r.length + count(...r); }
function guardRestStore(...r) { const kept = r; return count(...r) + kept.length; }
function guardRestReturn(flag, ...r) { return flag ? r : count(...r); }
for (let i = 0; i < WARM; i++) {
  assert.strictEqual(guardCall(three), 6);
  assert.strictEqual(guardArray(three).length, 3);
  assert.strictEqual(guardPrefix(0, three).length, 4);
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
assert.strictEqual(guardArray(quiet3).join(''), 'abcd');
assert.strictEqual(guardPrefix(0, quiet3).join(''), '0abcd');
assert.strictEqual(guardNew(quiet3).n, 4);
assert.strictEqual(guardMethod(quiet3), '10abc');
assert.strictEqual(guardSuper(quiet2).n, 4);
for (let i = 0; i < WARM; i++) {
  assert.strictEqual(guardCall(three), 6);
  assert.strictEqual(guardArray(three).length, 3);
  assert.strictEqual(guardNew(three).c, 3);
}
const sparse3 = [1, , 3];
assert.ok(Number.isNaN(guardCall(sparse3)));
assert.ok(1 in guardArray(sparse3));
assert.strictEqual(guardArray(sparse3)[1], undefined);
assert.strictEqual(guardNew(sparse3).b, undefined);
assert.strictEqual(guardNew(sparse3).n, 3);

class Loud extends Array {
  *[Symbol.iterator]() {
    log.push('loud');
    yield 'only';
  }
}
const loud = new Loud(1, 2, 3);
const own = [1, 2, 3];
own[Symbol.iterator] = function* () { log.push('own'); yield 'mine'; };
const getter = [1, 2, 3];
Object.defineProperty(getter, 1, { get() { log.push('get1'); return 'got'; }, enumerable: true, configurable: true });
const frozen = Object.freeze([1, 2, 3]);
for (let i = 0; i < WARM / 10; i++) {
  assert.strictEqual(spreadCount(loud), 1);
  assert.deepStrictEqual(spreadArray(loud), ['only']);
  assert.strictEqual(take(), 'loud,loud');
  assert.deepStrictEqual(spreadArray(own), ['mine']);
  assert.strictEqual(spreadCall(own), 'mineundefinedundefined');
  assert.strictEqual(take(), 'own,own');
  assert.deepStrictEqual(spreadArray(getter), [1, 'got', 3]);
  assert.strictEqual(spreadCall(getter), '1got3');
  assert.strictEqual(take(), 'get1,get1');
  assert.strictEqual(spreadCall(frozen), 6);
  assert.deepStrictEqual(spreadArray(frozen), [1, 2, 3]);
  assert.ok(!Object.isFrozen(spreadArray(frozen)));
}

const arrayIterator = Array.prototype[Symbol.iterator];
const arrayIteratorProto = Object.getPrototypeOf([][Symbol.iterator]());
const arrayIteratorNext = arrayIteratorProto.next;

arrayIteratorProto.next = function () {
  log.push('step');
  return arrayIteratorNext.call(this);
};
assert.strictEqual(spreadCall(three), 6);
assert.strictEqual(take(), 'step,step,step,step');
assert.strictEqual(spreadArray(two).join('|'), '4|5');
assert.strictEqual(take(), 'step,step,step');
assert.strictEqual(spreadArrayPrefix(0, two).join('|'), '0|4|5');
assert.strictEqual(take(), 'step,step,step');
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
assert.strictEqual(spreadArray(three).join('|'), 'p|q');
assert.strictEqual(spreadArrayPrefix(0, three).join('|'), '0|p|q');
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
assert.deepStrictEqual(spreadArray(three), [1, 2, 3]);
assert.strictEqual(forward(1, 2, 3), 6);
assert.strictEqual(makeRest(1, 2).b, 2);
assert.strictEqual(take(), '');
warm();
checkShapes();

let drainHits = 0;
function drainThenSpread(a, n) {
  for (;;) {
    if (n-- <= 0) break;
    drainHits++;
  }
  return [...a].length;
}
let drainTotal = 0;
for (let i = 0; i < 30000; i++) drainTotal += drainThenSpread(three, i % 3);
assert.strictEqual(drainTotal, 90000);
assert.strictEqual(drainHits, 30000);

const collectSeen = [];
function collectThenSpread(items, limit) {
  let n = 0;
  for (let guard = 0; guard < 1000; guard++) {
    if (n >= limit) break;
    collectSeen.push(n);
    n++;
  }
  return [...items, n];
}
let collectLast;
for (let i = 0; i < 30000; i++) {
  collectSeen.length = 0;
  collectLast = collectThenSpread(three, 2);
  if (collectSeen.length !== 2) break;
}
assert.deepStrictEqual(collectLast, [1, 2, 3, 2]);
assert.deepStrictEqual(collectSeen, [0, 1]);

let switchSide = 0;
function switchSpread(a, c) {
  let r = 0;
  switch (c) {
    case 0: switchSide++; break;
    case 1: r = [...a].length; break;
    default: switchSide += 2;
  }
  return r;
}
function switchSpreadLast(a, c) {
  let r = 0;
  switch (c) {
    case 0: switchSide++; break;
    default: switchSide += 2; break;
    case 1: r = [...a, c].length;
  }
  return r;
}
let switchTotal = 0;
for (let i = 0; i < 30000; i++) switchTotal += switchSpread(three, i % 3) + switchSpreadLast(three, i % 3);
assert.strictEqual(switchTotal, 70000);
assert.strictEqual(switchSide, 60000);

{
  const three = [1, 2, 3];
  const sink = [];
  function five(a, b, c, d, e) { return [a, b, c, d, e, arguments.length]; }
  function noted(tag, value) { sink.push(tag); return value; }
  function Made(a, b, c, d, e) { this.args = [a, b, c, d, e]; this.count = arguments.length; this.target = new.target; }
  class Base { constructor(a, b, c, d) { this.args = [a, b, c, d]; this.target = new.target; } }
  class Derived extends Base { constructor(arr, x) { super(...arr, x); this.own = x; } }
  class DerivedCall extends Base { constructor(arr, x) { super(...arr, noted('super', x)); } }
  const holder = { k: 7, method(a, b, c, d) { return [this.k, a, b, c, d]; } };

  function midCall(x, arr) { return five(x, ...arr, 9); }
  function midNew(arr, x) { return new Made(...arr, x, 8); }
  function midMethod(arr, x) { return holder.method(...arr, x); }
  function twoSpreads(arr, other) { return five(...arr, ...other); }
  function midSuper(arr, x) { return new Derived(arr, x); }
  function midSuperCall(arr, x) { return new DerivedCall(arr, x); }
  function midOrder(arr) { return five(noted('a', 1), ...noted('b', arr), noted('c', 9)); }
  function midMutate(arr) { return five(0, ...arr, arr.push(4)); }
  function midLate(arr) { return five(arr.length, ...arr, arr.length); }
  function midThrows(arr, x) { try { return five(x, ...arr, 1).length; } catch (error) { return error.message; } }
  function midNested(arr, x) { return five(x, ...arr, five(...arr, x, 0)[3]); }

  function checkMid(i) {
    assert.deepStrictEqual(midCall(i, three), [i, 1, 2, 3, 9, 5]);
    const made = midNew(three, i);
    assert.deepStrictEqual(made.args, [1, 2, 3, i, 8]);
    assert.strictEqual(made.count, 5);
    assert.strictEqual(made.target, Made);
    assert.deepStrictEqual(midMethod(three, i), [7, 1, 2, 3, i]);
    assert.deepStrictEqual(twoSpreads(three, three), [1, 2, 3, 1, 2, 6]);
    const derived = midSuper(three, i);
    assert.deepStrictEqual(derived.args, [1, 2, 3, i]);
    assert.strictEqual(derived.target, Derived);
    assert.strictEqual(derived.own, i);
    assert.deepStrictEqual(midSuperCall(three, i).args, [1, 2, 3, i]);
    assert.deepStrictEqual(midOrder(three), [1, 1, 2, 3, 9, 5]);
    assert.deepStrictEqual(sink.splice(0), ['super', 'a', 'b', 'c']);
    assert.deepStrictEqual(midMutate([1, 2, 3]), [0, 1, 2, 3, 4, 5]);
    assert.deepStrictEqual(midLate(three), [3, 1, 2, 3, 3, 5]);
    assert.strictEqual(midThrows(three, i), 6);
    assert.deepStrictEqual(midNested(three, i), [i, 1, 2, 3, i, 5]);
  }
  for (let i = 0; i < WARM; i++) checkMid(i);

  function joined(a, b, c) { return a + c; }
  function Kept(a, b, c) { this.sum = a + c; }
  function midJoined(x, arr) { return [joined(x, ...arr, 1), 'after']; }
  function midKept(x, arr) { return [new Kept(x, ...arr, 1).sum, 'after']; }
  function countAll() { return arguments.length; }
  function CountAll() { this.count = arguments.length; }
  const ten = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9];
  function midMany(a, b) { return countAll(...a, ...b, 1); }
  function midManyNew(a, b) { return new CountAll(...a, ...b, 1).count; }
  function midManyMethod(a, b) { return holder.method(...a, ...b, 1).length; }
  for (let i = 0; i < WARM; i++) {
    assert.deepStrictEqual(midJoined(i, [2]), [i + 1, 'after']);
    assert.deepStrictEqual(midKept(i, [2]), [i + 1, 'after']);
    assert.strictEqual(midMany(ten, ten), 21);
    assert.strictEqual(midManyNew(ten, ten), 21);
    assert.strictEqual(midManyMethod(ten, ten), 5);
    assert.strictEqual(midMany(three, three), 7);
  }
  assert.deepStrictEqual(midJoined('s', [2]), ['s1', 'after']);
  assert.deepStrictEqual(midKept('s', [2]), ['s1', 'after']);
  assert.deepStrictEqual(midJoined(1.5, [2]), [2.5, 'after']);
  assert.deepStrictEqual(midKept({ valueOf() { return 4; } }, [2]), [5, 'after']);
  for (let i = 0; i < WARM; i++) {
    assert.deepStrictEqual(midJoined(i, [2]), [i + 1, 'after']);
    assert.deepStrictEqual(midKept(i, [2]), [i + 1, 'after']);
  }

  assert.deepStrictEqual(midCall(0, [1, 2]), [0, 1, 2, 9, undefined, 4]);
  assert.deepStrictEqual(midCall(0, [1, 2, 3, 4, 5]), [0, 1, 2, 3, 4, 7]);
  assert.deepStrictEqual(midCall(0, []), [0, 9, undefined, undefined, undefined, 2]);
  assert.deepStrictEqual(midCall(0, [1, , 3]), [0, 1, undefined, 3, 9, 5]);
  assert.deepStrictEqual(midNew([1, , 3], 4).args, [1, undefined, 3, 4, 8]);
  assert.deepStrictEqual(midNew([], 4).args, [4, 8, undefined, undefined, undefined]);
  assert.deepStrictEqual(midMethod([1], 4), [7, 1, 4, undefined, undefined]);
  assert.deepStrictEqual(midSuper([1, , 3], 4).args, [1, undefined, 3, 4]);
  assert.deepStrictEqual(midSuper([5], 4).args, [5, 4, undefined, undefined]);
  assert.deepStrictEqual(midCall(0, 'ab'), [0, 'a', 'b', 9, undefined, 4]);
  assert.deepStrictEqual(midCall(0, new Set([4, 5])), [0, 4, 5, 9, undefined, 4]);
  assert.strictEqual(typeof midThrows(null, 0), 'string');
  assert.strictEqual(typeof midThrows(undefined, 0), 'string');
  assert.throws(() => midCall(0, 5), TypeError);
  assert.throws(() => midNew(5, 0), TypeError);
  assert.throws(() => midSuper({}, 0), TypeError);
  for (let i = 0; i < WARM; i++) checkMid(i);

  const proto = Object.create(Array.prototype);
  const odd = [1, 2, 3];
  Object.setPrototypeOf(odd, proto);
  proto[Symbol.iterator] = function* () { yield 'x'; yield 'y'; };
  assert.deepStrictEqual(midCall(0, odd), [0, 'x', 'y', 9, undefined, 4]);
  assert.deepStrictEqual(midNew(odd, 4).args, ['x', 'y', 4, 8, undefined]);
  assert.deepStrictEqual(midSuper(odd, 4).args, ['x', 'y', 4, undefined]);
  for (let i = 0; i < WARM; i++) checkMid(i);

  const iterator = Array.prototype[Symbol.iterator];
  Array.prototype[Symbol.iterator] = function* () { yield 'p'; };
  try {
    assert.deepStrictEqual(midCall(0, three), [0, 'p', 9, undefined, undefined, 3]);
    assert.deepStrictEqual(midNew(three, 4).args, ['p', 4, 8, undefined, undefined]);
    assert.deepStrictEqual(midMethod(three, 4), [7, 'p', 4, undefined, undefined]);
    assert.deepStrictEqual(twoSpreads(three, three), ['p', 'p', undefined, undefined, undefined, 2]);
    assert.deepStrictEqual(midSuper(three, 4).args, ['p', 4, undefined, undefined]);
  } finally {
    Array.prototype[Symbol.iterator] = iterator;
  }
  for (let i = 0; i < WARM; i++) checkMid(i);
}

console.log('ok');
