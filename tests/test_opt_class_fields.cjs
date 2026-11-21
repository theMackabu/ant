const assert = require('node:assert');

const WARM = 3000;
const log = [];
function take() { return log.splice(0).join(','); }
function note(tag, value) { log.push(tag); return value; }
function own(object, key) { return Object.getOwnPropertyDescriptor(object, key); }
function plain(object, key, value) {
  const found = own(object, key);
  assert.ok(found, String(key));
  assert.strictEqual(found.value, value);
  assert.strictEqual(found.writable, true);
  assert.strictEqual(found.enumerable, true);
  assert.strictEqual(found.configurable, true);
}

class Pair { x = 1; y = 2 }
class Triple extends Pair { z = 3 }
class Body {
  x = 1;
  y = 2;
  constructor(a) { this.a = a; }
}
class DerivedBody extends Body {
  z = this.a + 1;
  constructor(a, b) {
    super(a);
    this.b = b;
  }
}
class Chain {
  a = 1;
  b = this.a + 1;
  c = [this.a, this.b];
  d = { sum: this.a + this.b };
  e = this.c.length + this.d.sum;
}
class Wide { a = 1; b = 2; c = 3; d = 4; e = 5; f = 6; g = 7; h = 8 }
class Wider extends Wide { i = 9; j = 10; k = 11; l = 12; m = 13; n = 14; o = 15; p = 16; q = 17; r = 18; s = 19; t = 20 }
class Empty { value; other }
class Arrow {
  tag = 'arrow';
  get = () => this.tag;
  self = () => this;
}
class Quoted { 'spaced key' = 1; "double" = 2; 'x' = 3 }
class Values {
  n = null;
  u = undefined;
  t = true;
  f = 1.5;
  s = 'str';
  big = 2 ** 40;
  neg = -0;
  fn = Math.max;
  sym = Symbol.iterator;
}

function makePair() { return new Pair(); }
function makeTriple() { return new Triple(); }
function makeBody(a) { return new Body(a); }
function makeDerivedBody(a, b) { return new DerivedBody(a, b); }
function makeChain() { return new Chain(); }
function makeWide() { return new Wide(); }
function makeWider() { return new Wider(); }
function sumPair() {
  const made = new Pair();
  return made.x + made.y;
}

const early = [makePair(), makeTriple(), makeBody(1), makeDerivedBody(1, 2), makeChain(), makeWide(), makeWider(), new Empty(), new Arrow(), new Quoted(), new Values()];

function checkBasics() {
  const pair = makePair();
  assert.deepStrictEqual(Object.keys(pair), ['x', 'y']);
  plain(pair, 'x', 1);
  plain(pair, 'y', 2);
  assert.strictEqual(Object.getPrototypeOf(pair), Pair.prototype);
  assert.notStrictEqual(makePair(), makePair());
  assert.strictEqual(sumPair(), 3);

  const triple = makeTriple();
  assert.deepStrictEqual(Object.keys(triple), ['x', 'y', 'z']);
  plain(triple, 'z', 3);
  assert.ok(triple instanceof Pair);

  const body = makeBody('arg');
  assert.deepStrictEqual(Object.keys(body), ['x', 'y', 'a']);
  assert.strictEqual(body.a, 'arg');

  const derived = makeDerivedBody(4, 'b');
  assert.deepStrictEqual(Object.keys(derived), ['x', 'y', 'a', 'z', 'b']);
  assert.strictEqual(derived.z, 5);
  assert.strictEqual(makeDerivedBody('s', 0).z, 's1');

  const chain = makeChain();
  assert.deepStrictEqual(Object.keys(chain), ['a', 'b', 'c', 'd', 'e']);
  assert.deepStrictEqual(chain.c, [1, 2]);
  assert.deepStrictEqual(chain.d, { sum: 3 });
  assert.strictEqual(chain.e, 5);
  assert.notStrictEqual(makeChain().c, makeChain().c);

  const wide = makeWide();
  assert.deepStrictEqual(Object.keys(wide), ['a', 'b', 'c', 'd', 'e', 'f', 'g', 'h']);
  assert.strictEqual(wide.a + wide.h, 9);
  const wider = makeWider();
  assert.strictEqual(Object.keys(wider).join(''), 'abcdefghijklmnopqrst');
  assert.strictEqual(wider.t, 20);
  assert.strictEqual(wider.i + wider.a, 10);
  wider.extra = 21;
  assert.strictEqual(wider.extra, 21);
  assert.strictEqual(Object.keys(wider).length, 21);

  const empty = new Empty();
  assert.deepStrictEqual(Object.keys(empty), ['value', 'other']);
  plain(empty, 'value', undefined);

  const arrow = new Arrow();
  assert.strictEqual(arrow.get(), 'arrow');
  assert.strictEqual(arrow.self(), arrow);
  assert.strictEqual(arrow.self.call(null), arrow);
  assert.notStrictEqual(arrow.get, new Arrow().get);

  const quoted = new Quoted();
  assert.deepStrictEqual(Object.keys(quoted), ['spaced key', 'double', 'x']);
  assert.strictEqual(quoted['spaced key'] + quoted.double + quoted.x, 6);

  const values = new Values();
  assert.strictEqual(values.n, null);
  assert.strictEqual(values.u, undefined);
  assert.ok('u' in values);
  assert.strictEqual(values.t, true);
  assert.strictEqual(values.f, 1.5);
  assert.strictEqual(values.s, 'str');
  assert.strictEqual(values.big, 1099511627776);
  assert.ok(Object.is(values.neg, -0));
  assert.strictEqual(values.fn, Math.max);
  assert.strictEqual(values.sym, Symbol.iterator);
}

for (let i = 0; i < WARM; i++) {
  assert.strictEqual(makePair().y, 2);
  assert.strictEqual(makeTriple().z, 3);
  assert.strictEqual(makeBody(i).a, i);
  assert.strictEqual(makeDerivedBody(i, 1).z, i + 1);
  assert.strictEqual(makeChain().e, 5);
  assert.strictEqual(makeWide().h, 8);
  assert.strictEqual(makeWider().t, 20);
  assert.strictEqual(sumPair(), 3);
  assert.strictEqual(new Arrow().get(), 'arrow');
  assert.strictEqual(new Values().f, 1.5);
}
checkBasics();

const late = [makePair(), makeTriple(), makeBody(1), makeDerivedBody(1, 2), makeChain(), makeWide(), makeWider(), new Empty(), new Arrow(), new Quoted(), new Values()];
for (let i = 0; i < early.length; i++) {
  assert.deepStrictEqual(Object.keys(early[i]), Object.keys(late[i]));
  assert.deepStrictEqual(Object.getOwnPropertyNames(early[i]), Object.getOwnPropertyNames(late[i]));
  assert.strictEqual(JSON.stringify(early[i]), JSON.stringify(late[i]));
  const names = [];
  for (const name in late[i]) names.push(name);
  assert.deepStrictEqual(names, Object.keys(early[i]));
  for (const key of Object.getOwnPropertyNames(late[i])) {
    const found = own(late[i], key);
    assert.strictEqual(found.writable, true, key);
    assert.strictEqual(found.enumerable, true, key);
    assert.strictEqual(found.configurable, true, key);
  }
}

class Order {
  first = note('first', 1);
  second = note('second', this.first + 1);
  constructor() { note('body', 0); }
}
class OrderDerived extends Order {
  third = note('third', this.second + 1);
  constructor() {
    note('before', 0);
    super();
    note('after', this.third);
  }
}
class OrderDefault extends Order {
  fourth = note('fourth', 4);
}
for (let i = 0; i < WARM; i++) {
  assert.strictEqual(new Order().second, 2);
  assert.strictEqual(take(), 'first,second,body');
  assert.strictEqual(new OrderDerived().third, 3);
  assert.strictEqual(take(), 'before,first,second,body,third,after');
  assert.strictEqual(new OrderDefault().fourth, 4);
  assert.strictEqual(take(), 'first,second,body,fourth');
}

let fail = false;
class Thrower {
  a = note('a', 1);
  b = fail ? (() => { throw new RangeError('field'); })() : note('b', 2);
  c = note('c', 3);
}
class ThrowerDerived extends Thrower {
  d = note('d', 4);
}
let seen = null;
class Leak {
  a = (seen = this, 1);
  b = fail ? (() => { throw new RangeError('leak'); })() : 2;
  c = 3;
}
for (let i = 0; i < WARM; i++) {
  assert.strictEqual(new Thrower().c, 3);
  assert.strictEqual(take(), 'a,b,c');
  assert.strictEqual(new ThrowerDerived().d, 4);
  assert.strictEqual(take(), 'a,b,c,d');
  assert.strictEqual(new Leak().c, 3);
}
fail = true;
for (let i = 0; i < 20; i++) {
  assert.throws(() => new Thrower(), RangeError);
  assert.strictEqual(take(), 'a');
  assert.throws(() => new ThrowerDerived(), RangeError);
  assert.strictEqual(take(), 'a');
  assert.throws(() => new Leak(), RangeError);
  assert.deepStrictEqual(Object.keys(seen), ['a']);
}
fail = false;
assert.strictEqual(new Thrower().c, 3);
assert.strictEqual(take(), 'a,b,c');
assert.deepStrictEqual(Object.keys(new Leak()), ['a', 'b', 'c']);

class Names {
  __proto__ = 'proto-field';
  prototype = 'prototype-field';
  length = 'length-field';
  next = 'next-field';
  return = 'return-field';
  then = 'then-field';
  name = 'name-field';
  toString = 'string-field';
  valueOf = 'value-field';
  hasOwnProperty = 'has-field';
}
for (let i = 0; i < WARM; i++) assert.strictEqual(new Names().then, 'then-field');
{
  const made = new Names();
  assert.deepStrictEqual(Object.keys(made), ['__proto__', 'prototype', 'length', 'next', 'return', 'then', 'name', 'toString', 'valueOf', 'hasOwnProperty']);
  for (const name of Object.keys(made)) plain(made, name, name === '__proto__' ? 'proto-field' : name === 'toString' ? 'string-field' : name === 'valueOf' ? 'value-field' : name === 'hasOwnProperty' ? 'has-field' : name + '-field');
  assert.strictEqual(Object.getPrototypeOf(made), Names.prototype);
}

class Accessors {
  set x(value) { log.push('setter'); }
  get y() { log.push('getter'); return 'proto'; }
}
Object.defineProperty(Accessors.prototype, 'z', { value: 'locked', writable: false, enumerable: false, configurable: false });
class Shadow extends Accessors { x = 1; y = 2; z = 3 }
class Lazy { x = 1; y = 2 }
class LazyDerived extends Lazy { z = 3 }
for (let i = 0; i < WARM; i++) {
  const made = new Shadow();
  assert.strictEqual(made.x + made.y + made.z, 6);
  assert.strictEqual(new LazyDerived().z, 3);
}
assert.strictEqual(take(), '');
{
  const made = new Shadow();
  plain(made, 'x', 1);
  plain(made, 'y', 2);
  plain(made, 'z', 3);
  assert.deepStrictEqual(Object.keys(made), ['x', 'y', 'z']);
}
Object.defineProperty(Lazy.prototype, 'x', { set(value) { log.push('late-setter'); }, configurable: true });
Object.defineProperty(Lazy.prototype, 'z', { get() { log.push('late-getter'); return 0; }, set(value) { log.push('late-setter'); }, configurable: true });
Object.defineProperty(Object.prototype, 'y', { set(value) { log.push('object-setter'); }, configurable: true });
for (let i = 0; i < 50; i++) {
  const made = new LazyDerived();
  plain(made, 'x', 1);
  plain(made, 'y', 2);
  plain(made, 'z', 3);
  plain(new Lazy(), 'y', 2);
  plain(makePair(), 'y', 2);
}
delete Object.prototype.y;
assert.strictEqual(take(), '');

let mode = 'plain';
const traps = [];
class Returns {
  constructor() {
    if (mode === 'other') return { other: true };
    if (mode === 'frozen') return Object.freeze({ kept: 1 });
    if (mode === 'sealed') return Object.seal({ kept: 1 });
    if (mode === 'closed') return Object.preventExtensions({ kept: 1 });
    if (mode === 'existing') return { x: 'old', y: 'old', z: 'old' };
    if (mode === 'hidden') return Object.defineProperty({}, 'x', { value: 'hidden', writable: true, enumerable: false, configurable: true });
    if (mode === 'accessor') return Object.defineProperty({}, 'x', { get() { log.push('own-getter'); return 0; }, set(value) { log.push('own-setter'); }, configurable: true });
    if (mode === 'locked') return Object.defineProperty({}, 'x', { value: 'locked', writable: true, enumerable: true, configurable: false });
    if (mode === 'array') return [7, 8];
    if (mode === 'function') return function made() {};
    if (mode === 'proxy') {
      return new Proxy({}, {
        defineProperty(target, name, descriptor) {
          traps.push([name, descriptor.value, descriptor.writable, descriptor.enumerable, descriptor.configurable].join(':'));
          return Reflect.defineProperty(target, name, descriptor);
        },
        set() { traps.push('set'); return true; },
      });
    }
    if (mode === 'refuse') return new Proxy({}, { defineProperty() { return false; } });
  }
}
class FromReturns extends Returns { x = 1; y = 2 }
function makeFromReturns() { return new FromReturns(); }
for (let i = 0; i < WARM; i++) assert.strictEqual(makeFromReturns().y, 2);
function checkReturns() {
  mode = 'other';
  let made = makeFromReturns();
  assert.deepStrictEqual(Object.keys(made), ['other', 'x', 'y']);
  assert.ok(!(made instanceof FromReturns));
  for (const closed of ['frozen', 'sealed', 'closed']) {
    mode = closed;
    assert.throws(makeFromReturns, TypeError);
  }
  mode = 'existing';
  made = makeFromReturns();
  assert.deepStrictEqual(Object.keys(made), ['x', 'y', 'z']);
  plain(made, 'x', 1);
  plain(made, 'y', 2);
  assert.strictEqual(made.z, 'old');
  mode = 'hidden';
  made = makeFromReturns();
  plain(made, 'x', 1);
  assert.deepStrictEqual(Object.keys(made), ['x', 'y']);
  mode = 'accessor';
  made = makeFromReturns();
  plain(made, 'x', 1);
  assert.strictEqual(take(), '');
  mode = 'locked';
  assert.throws(makeFromReturns, TypeError);
  mode = 'array';
  made = makeFromReturns();
  assert.ok(Array.isArray(made));
  assert.strictEqual(made.length, 2);
  assert.deepStrictEqual(Object.keys(made), ['0', '1', 'x', 'y']);
  mode = 'function';
  made = makeFromReturns();
  assert.strictEqual(typeof made, 'function');
  plain(made, 'y', 2);
  mode = 'proxy';
  made = makeFromReturns();
  assert.deepStrictEqual(traps.splice(0), ['x:1:true:true:true', 'y:2:true:true:true']);
  assert.deepStrictEqual(Object.keys(made), ['x', 'y']);
  mode = 'refuse';
  assert.throws(makeFromReturns, TypeError);
  mode = 'plain';
  made = makeFromReturns();
  assert.deepStrictEqual(Object.keys(made), ['x', 'y']);
  assert.ok(made instanceof FromReturns);
}
checkReturns();
for (let i = 0; i < WARM; i++) assert.strictEqual(makeFromReturns().y, 2);
checkReturns();

const singleton = { x: 0, y: 0 };
class Single { constructor() { return singleton; } }
class OverSingle extends Single { x = 1; y = 2 }
function makeSingle() { return new OverSingle(); }
for (let i = 0; i < WARM; i++) assert.strictEqual(makeSingle(), singleton);
plain(singleton, 'x', 1);
Object.defineProperty(singleton, 'x', { value: 'hidden', enumerable: false });
assert.deepStrictEqual(Object.keys(singleton), ['y']);
makeSingle();
plain(singleton, 'x', 1);
assert.deepStrictEqual(Object.keys(singleton), ['x', 'y']);
Object.defineProperty(singleton, 'y', { get() { log.push('single-getter'); return 0; }, set(value) { log.push('single-setter'); }, configurable: true });
makeSingle();
plain(singleton, 'y', 2);
assert.strictEqual(take(), '');
Object.defineProperty(singleton, 'x', { value: 'fixed', writable: false });
makeSingle();
plain(singleton, 'x', 1);
Object.defineProperty(singleton, 'y', { configurable: false });
assert.throws(makeSingle, TypeError);
Object.freeze(singleton);
assert.throws(makeSingle, TypeError);

class Lock {
  a = 1;
  b = fail ? Object.preventExtensions(this) : 2;
  c = 3;
}
class Grow {
  a = (this.early = 0, 1);
  b = 2;
}
class Remove {
  a = 1;
  b = (fail ? delete this.a : 0, 2);
  c = 3;
}
for (let i = 0; i < WARM; i++) {
  assert.strictEqual(new Lock().c, 3);
  assert.strictEqual(new Grow().b, 2);
  assert.strictEqual(new Remove().c, 3);
}
assert.deepStrictEqual(Object.keys(new Grow()), ['early', 'a', 'b']);
fail = true;
assert.throws(() => new Lock(), TypeError);
assert.deepStrictEqual(Object.keys(new Remove()), ['b', 'c']);
fail = false;
assert.deepStrictEqual(Object.keys(new Lock()), ['a', 'b', 'c']);
assert.deepStrictEqual(Object.keys(new Remove()), ['a', 'b', 'c']);

class Parent { shared = 'parent'; only = 1 }
class Child extends Parent { shared = 'child'; mine = 2 }
class GrandChild extends Child { shared = 'grand'; only = 'grand-only' }
for (let i = 0; i < WARM; i++) {
  assert.strictEqual(new Child().shared, 'child');
  assert.strictEqual(new GrandChild().only, 'grand-only');
}
assert.deepStrictEqual(Object.keys(new Child()), ['shared', 'only', 'mine']);
assert.deepStrictEqual(Object.keys(new GrandChild()), ['shared', 'only', 'mine']);
plain(new GrandChild(), 'shared', 'grand');
assert.strictEqual(new Parent().shared, 'parent');

class Root { kind = 'root'; level = 0 }
class LeafA extends Root { a = 1 }
class LeafB extends Root { b = 2; constructor() { super(); this.extra = 'b'; } }
class LeafC extends Root { c = 3 }
class LeafD extends LeafA { d = 4 }
class LeafE extends Root { e = 5 }
const leaves = [Root, LeafA, LeafB, LeafC, LeafD, LeafE];
function makeLeaf(index) { return new leaves[index](); }
for (let i = 0; i < WARM * 2; i++) assert.strictEqual(makeLeaf(i % leaves.length).kind, 'root');
assert.deepStrictEqual(Object.keys(makeLeaf(0)), ['kind', 'level']);
assert.deepStrictEqual(Object.keys(makeLeaf(1)), ['kind', 'level', 'a']);
assert.deepStrictEqual(Object.keys(makeLeaf(2)), ['kind', 'level', 'b', 'extra']);
assert.deepStrictEqual(Object.keys(makeLeaf(3)), ['kind', 'level', 'c']);
assert.deepStrictEqual(Object.keys(makeLeaf(4)), ['kind', 'level', 'a', 'd']);
assert.deepStrictEqual(Object.keys(makeLeaf(5)), ['kind', 'level', 'e']);
function PlainBase() { this.viaFunction = true; }
class OverFunction extends PlainBase { field = 1 }
function makeNull() { return Reflect.construct(Pair, [], NullTarget); }
function NullTarget() {}
NullTarget.prototype = null;
for (let i = 0; i < WARM; i++) {
  assert.strictEqual(new OverFunction().field, 1);
  assert.strictEqual(makeNull().y, 2);
}
assert.deepStrictEqual(Object.keys(new OverFunction()), ['viaFunction', 'field']);
assert.strictEqual(Object.getPrototypeOf(makeNull()), Object.prototype);
assert.strictEqual(Object.getPrototypeOf(Reflect.construct(Pair, [], Triple)), Triple.prototype);
assert.deepStrictEqual(Object.keys(Reflect.construct(Pair, [], Triple)), ['x', 'y']);

class Private {
  #a = 1;
  #b = this.#a + 1;
  c = this.#b + 1;
  #unset;
  static has(object) { return #a in object; }
  static unset(object) { return object.#unset; }
  sum() { return this.#a + this.#b + this.c; }
  bump() { this.#a++; return this.#a; }
}
class PrivateDerived extends Private {
  #a = 10;
  d = this.sum();
  mine() { return this.#a; }
}
for (let i = 0; i < WARM; i++) {
  const made = new Private();
  assert.strictEqual(made.sum(), 6);
  assert.strictEqual(new PrivateDerived().d, 6);
}
{
  const made = new PrivateDerived();
  assert.deepStrictEqual(Object.keys(made), ['c', 'd']);
  assert.strictEqual(made.mine(), 10);
  assert.strictEqual(made.bump(), 2);
  assert.strictEqual(made.mine(), 10);
  assert.strictEqual(Private.has(made), true);
  assert.strictEqual(Private.has({}), false);
  assert.strictEqual(Private.has(Object.create(made)), false);
  assert.strictEqual(Private.unset(made), undefined);
  assert.throws(() => Private.unset({}), TypeError);
  assert.throws(() => Private.has(1), TypeError);
  assert.throws(() => Private.prototype.sum.call({}), TypeError);
}

class List extends Array { label = 'list'; size = this.length }
class Failure extends Error { code = 42; detail = this.message + '!' }
class Table extends Map { tag = 'table' }
class Callable extends Function { field = 'callable' }
for (let i = 0; i < WARM / 10; i++) {
  assert.strictEqual(new List(3).label, 'list');
  assert.strictEqual(new Failure('bad').code, 42);
  assert.strictEqual(new Table().tag, 'table');
}
{
  const list = new List(3);
  assert.ok(Array.isArray(list));
  assert.strictEqual(list.length, 3);
  assert.strictEqual(list.size, 3);
  assert.deepStrictEqual(Object.keys(list), ['label', 'size']);
  list.push(1);
  assert.strictEqual(list.length, 4);
  assert.ok(list.map((value) => value) instanceof List);
  const failure = new Failure('bad');
  assert.strictEqual(failure.detail, 'bad!');
  assert.strictEqual(failure.message, 'bad');
  assert.ok(failure instanceof Error);
  plain(failure, 'code', 42);
  const table = new Table([[1, 2]]);
  assert.strictEqual(table.get(1), 2);
  plain(table, 'tag', 'table');
  plain(new Callable(), 'field', 'callable');
}

class Rebind { value = 1 }
function makeRebind() { return new Rebind(); }
for (let i = 0; i < WARM; i++) assert.strictEqual(makeRebind().value, 1);
const before = makeRebind();
Rebind.prototype.added = 'late';
Object.setPrototypeOf(Rebind.prototype, { inherited: true });
const after = makeRebind();
assert.deepStrictEqual(Object.keys(after), ['value']);
assert.strictEqual(after.added, 'late');
assert.strictEqual(after.inherited, true);
assert.strictEqual(before.inherited, true);
let rebindSetterRuns = 0;
Object.setPrototypeOf(Rebind.prototype, { get value() { return 'inherited'; }, set value(v) { rebindSetterRuns++; } });
for (let i = 0; i < WARM; i++) plain(makeRebind(), 'value', 1);
assert.strictEqual(rebindSetterRuns, 0);
assert.strictEqual(Object.getPrototypeOf(makeRebind()), Object.getPrototypeOf(after));
checkBasics();

class Fanned { a = 1; b = 2 }
const fannedKinds = [];
for (let k = 0; k < 40; k++) fannedKinds.push(class extends Fanned { });
function makeFanned(i) { return new fannedKinds[i % fannedKinds.length](); }
for (let i = 0; i < WARM * 4; i++) {
  const made = makeFanned(i);
  plain(made, 'a', 1);
  plain(made, 'b', 2);
  assert.deepStrictEqual(Object.keys(made), ['a', 'b']);
  assert.strictEqual(Object.getPrototypeOf(made), fannedKinds[i % fannedKinds.length].prototype);
}

class HandsBack { constructor(target) { return target; } }
class Stamper extends HandsBack { mark = 1; side = 2 }
const stamperTargets = [
  () => [], () => function () {}, () => new Uint8Array(1), () => new Proxy({}, {}),
  () => ({}), () => ({ mark: 0 }), () => Object.create(null), () => new Map(),
];
function stampOne(i) {
  const target = stamperTargets[i % stamperTargets.length]();
  new Stamper(target);
  return target;
}
for (let i = 0; i < WARM * 4; i++) {
  const target = stampOne(i);
  plain(target, 'mark', 1);
  plain(target, 'side', 2);
}
assert.throws(() => new Stamper(Object.freeze([])), TypeError);
assert.throws(() => new Stamper(Object.preventExtensions(function () {})), TypeError);
assert.throws(() => new Stamper(Object.seal({ mark: 0 })), TypeError);
let stamperSetterRuns = 0;
const stamped = stampOne(4);
Object.defineProperty(stamped, 'mark', { get() { return 'get'; }, set(v) { stamperSetterRuns++; }, configurable: true });
new Stamper(stamped);
plain(stamped, 'mark', 1);
assert.strictEqual(stamperSetterRuns, 0);

class ArrayOnly extends Array { tag = 'array'; size = this.length }
function makeArrayOnly(n) { return new ArrayOnly(n); }
for (let i = 0; i < WARM * 2; i++) {
  const made = makeArrayOnly(3);
  plain(made, 'tag', 'array');
  plain(made, 'size', 3);
  assert.strictEqual(made.length, 3);
}

class GivesBack { constructor(target) { return target; } }
class Indexed extends GivesBack { 0 = 'zero'; '1' = 'one'; mark = 'mark' }
function indexOne(target) {
  try { new Indexed(target); return 'ok'; } catch (error) { return error instanceof TypeError ? 'type' : 'other'; }
}
const indexTargets = [() => ({}), () => new String('ab'), () => ({ p: 1 }), () => new String('abc'), () => [], () => Object.create(String.prototype)];
const indexWant = ['ok', 'type', 'ok', 'type', 'ok', 'ok'];
for (let i = 0; i < WARM * 4; i++) {
  const kind = i % indexTargets.length;
  const target = indexTargets[kind]();
  assert.strictEqual(indexOne(target), indexWant[kind]);
  if (indexWant[kind] === 'ok') {
    plain(target, '0', 'zero');
    plain(target, '1', 'one');
    plain(target, 'mark', 'mark');
  } else {
    assert.strictEqual(target[0], 'a');
    assert.strictEqual(own(target, 'mark'), undefined);
  }
}

class QuotedIndex extends GivesBack { '1' = 'one' }
function quoteOne(target) {
  try { new QuotedIndex(target); return target[1]; } catch (error) { return error instanceof TypeError ? 'type' : 'other'; }
}
const quoteTargets = [() => ({}), () => new String('ab'), () => [], () => new String('a')];
const quoteWant = ['one', 'type', 'one', 'one'];
for (let i = 0; i < WARM * 4; i++) {
  const kind = i % quoteTargets.length;
  assert.strictEqual(quoteOne(quoteTargets[kind]()), quoteWant[kind]);
}

class Sized extends GivesBack { length = 7 }
function sizeOne(target) {
  try { new Sized(target); return target.length; } catch (error) { return error instanceof TypeError ? 'type' : 'other'; }
}
const sizeTargets = [() => ({}), () => new String('ab'), () => Object.create(String.prototype), () => [1], () => new Number(1)];
const sizeWant = [7, 'type', 7, 'type', 7];
for (let i = 0; i < WARM * 4; i++) {
  const kind = i % sizeTargets.length;
  assert.strictEqual(sizeOne(sizeTargets[kind]()), sizeWant[kind]);
}

console.log('ok');
