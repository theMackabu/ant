const assert = require('node:assert');

const WARM = 3000;
const log = [];
function take() { return log.splice(0).join(','); }

function spreadNew(arr) { return new Point(...arr); }
function spreadNewFixed(x, arr) { return new Point(x, ...arr); }

function Point(a, b, c) {
  this.a = a;
  this.b = b;
  this.c = c;
  this.n = arguments.length;
  this.target = new.target;
}

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
class PrefixDerived extends Base {
  constructor(tag, ...rest) { super(...rest); this.tag = tag; }
}
class FixedDerived extends Base {
  constructor(a, ...rest) { super(a, ...rest); }
}
class ArrayDerived extends Base {
  constructor(arr) { super(...arr); }
}
function makePure(a, b) { return new PureRestDerived(a, b); }
function makePrefix(t, a, b) { return new PrefixDerived(t, a, b); }
function makeFixed(a, b) { return new FixedDerived(a, b); }
function makeArray(arr) { return new ArrayDerived(arr); }

const three = [1, 2, 3];
const two = [4, 5];

function warm() {
  for (let i = 0; i < WARM; i++) {
    assert.strictEqual(spreadNew(three).c, 3);
    assert.strictEqual(spreadNewFixed(0, two).c, 5);
    assert.strictEqual(makePure(1, 2).b, 2);
    assert.strictEqual(makePrefix('t', 1, 2).b, 2);
    assert.strictEqual(makeFixed(1, 2).b, 2);
    assert.strictEqual(makeArray(two).b, 5);
  }
}
warm();

function checkShapes() {
  assert.throws(() => spreadNew({}), TypeError);

  assert.strictEqual(spreadNew(three).n, 3);
  assert.strictEqual(spreadNew(three).target, Point);
  assert.strictEqual(spreadNew([]).n, 0);
  assert.strictEqual(spreadNew(new Set([1, 2])).b, 2);
  assert.strictEqual(spreadNew(new Array(20).fill(3)).n, 20);
  assert.strictEqual(spreadNewFixed(1, []).n, 1);
  assert.strictEqual(spreadNewFixed(1, three).n, 4);

  assert.strictEqual(new PureRestDerived().n, 0);
  assert.strictEqual(new PureRestDerived(1, 2, 3).n, 3);
  assert.strictEqual(new PureRestDerived(...new Array(30).fill(1)).n, 30);
  assert.strictEqual(makePrefix('t', 1, 2).tag, 't');
  assert.strictEqual(makePrefix('t', 1, 2).n, 2);
  assert.strictEqual(new PrefixDerived().n, 0);
  assert.strictEqual(new FixedDerived().n, 1);
  assert.strictEqual(makeArray(two).a, 4);
  assert.strictEqual(makeArray([]).n, 0);
  assert.strictEqual(makeArray(new Set([7, 8, 9])).n, 3);
  assert.strictEqual(makeArray('xy').b, 'y');
  assert.throws(() => makeArray(3), TypeError);
}
checkShapes();
warm();
checkShapes();

assert.strictEqual(RestDerived.length, 0);
assert.strictEqual(PrefixDerived.length, 1);

class DeoptBase {
  constructor(v) { this.v = v + 1; }
}
class DeoptDerived extends DeoptBase {
  constructor(...args) { super(...args); this.k = args.length; }
}
function deoptNew(v) { return new DeoptDerived(v, 0); }
for (let i = 0; i < WARM; i++) {
  assert.strictEqual(deoptNew(i).v, i + 1);
}
assert.strictEqual(deoptNew('s').v, 's1');
assert.strictEqual(deoptNew('s').k, 2);
for (let i = 0; i < WARM; i++) {
  assert.strictEqual(deoptNew(i).k, 2);
}

function Other(a, b) {
  this.other = [a, b];
}
class Retarget extends Base {
  constructor(...args) { super(...args); }
}
function makeRetarget(a, b) { return new Retarget(a, b); }
class Returns {
  constructor(a, b) { return { made: [a, b], target: new.target }; }
}
class ReturnsDerived extends Returns {
  constructor(...args) { super(...args); this.extra = args.length; }
}
function makeReturns(a, b) { return new ReturnsDerived(a, b); }
for (let i = 0; i < WARM; i++) {
  assert.strictEqual(makeRetarget(1, 2).b, 2);
  const made = makeReturns(1, 2);
  assert.deepStrictEqual(made.made, [1, 2]);
  assert.strictEqual(made.extra, 2);
  assert.strictEqual(made.target, ReturnsDerived);
  assert.ok(!(made instanceof ReturnsDerived));
}
Object.setPrototypeOf(Retarget, Other);
for (let i = 0; i < WARM / 10; i++) {
  const made = makeRetarget(1, 2);
  assert.deepStrictEqual(made.other, [1, 2]);
  assert.strictEqual(made.b, undefined);
  assert.ok(made instanceof Retarget);
}
Object.setPrototypeOf(Retarget, Base);
assert.strictEqual(makeRetarget(1, 2).b, 2);
assert.strictEqual(Reflect.construct(RestDerived, [1, 2], PureRestDerived).target, PureRestDerived);
assert.ok(Reflect.construct(spreadNew.bind(null, three), []) instanceof Point);
assert.throws(() => RestDerived(1, 2), TypeError);

const arrayIterator = Array.prototype[Symbol.iterator];
const arrayIteratorProto = Object.getPrototypeOf([][Symbol.iterator]());
const arrayIteratorNext = arrayIteratorProto.next;

arrayIteratorProto.next = function () {
  log.push('step');
  return arrayIteratorNext.call(this);
};
assert.strictEqual(makePure(1, 2).b, 2);
assert.strictEqual(take(), 'step,step,step');
assert.strictEqual(makePrefix('t', 1, 2).b, 2);
assert.strictEqual(take(), 'step,step,step');
assert.strictEqual(new PureRestDerived(1).n, 1);
assert.strictEqual(take(), 'step,step');
assert.strictEqual(spreadNew(three).n, 3);
assert.strictEqual(take(), 'step,step,step,step');
arrayIteratorProto.next = arrayIteratorNext;

Array.prototype[Symbol.iterator] = function* () {
  log.push('iterate');
  yield 'p';
  yield 'q';
};
assert.strictEqual(new RestDerived(1, 2, 3).own, 3);
assert.strictEqual(makePure(1, 2).a, 'p');
assert.strictEqual(new PureRestDerived().n, 2);
assert.strictEqual(makePrefix('t', 1, 2).b, 'q');
assert.strictEqual(makeArray(two).a, 'p');
assert.strictEqual(spreadNew(three).n, 2);
Array.prototype[Symbol.iterator] = arrayIterator;
assert.ok(take().length > 0);
warm();
checkShapes();

console.log('ok');
