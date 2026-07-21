import { test, summary } from './helpers.js';

const HOT = 5000;

console.log('monomorphic instance methods\n');

class Point {
  constructor(x, y) {
    this.x = x;
    this.y = y;
  }
  sum() {
    return this.x + this.y;
  }
  scaledSum(k) {
    return (this.x + this.y) * k;
  }
  classify() {
    if (this.x > this.y) return 'x';
    if (this.x < this.y) return 'y';
    return 'eq';
  }
}

function driveSum(p) {
  let acc = 0;
  for (let i = 0; i < 8; i++) acc += p.sum();
  return acc;
}

let sumAcc = 0;
for (let i = 0; i < HOT; i++) sumAcc += driveSum(new Point(3, 4));
test('monomorphic method inlined returns correct sum', sumAcc, HOT * 8 * 7);

function driveArgs(p, k) {
  return p.scaledSum(k);
}
let argsAcc = 0;
for (let i = 0; i < HOT; i++) argsAcc += driveArgs(new Point(2, 5), 3);
test('inlined method with argument', argsAcc, HOT * (2 + 5) * 3);

function driveBranch(p) {
  return p.classify();
}
let branchOk = true;
for (let i = 0; i < HOT; i++) {
  if (driveBranch(new Point(1, 9)) !== 'y') branchOk = false;
  if (driveBranch(new Point(9, 1)) !== 'x') branchOk = false;
  if (driveBranch(new Point(4, 4)) !== 'eq') branchOk = false;
}
test('inlined method with control flow', branchOk, true);

console.log('\nplain-object prototype methods\n');

const proto = {
  twice() {
    return this.v * 2;
  }
};
function driveProto(o) {
  return o.twice();
}
let protoAcc = 0;
for (let i = 0; i < HOT; i++) {
  const o = Object.create(proto);
  o.v = i & 7;
  protoAcc += driveProto(o);
}
let protoRef = 0;
for (let i = 0; i < HOT; i++) protoRef += (i & 7) * 2;
test('prototype method devirtualized on plain object', protoAcc, protoRef);

console.log('\npolymorphic call sites fall back correctly\n');

class A {
  who() {
    return 1;
  }
}
class B {
  who() {
    return 2;
  }
}
function drivePoly(o) {
  return o.who();
}

let polyAcc = 0;
for (let i = 0; i < HOT; i++) {
  polyAcc += drivePoly(i % 2 === 0 ? new A() : new B());
}
test('polymorphic site returns per-receiver result', polyAcc, (HOT / 2) * 1 + (HOT / 2) * 2);

console.log('\nsuper method calls stay correct\n');

class Base {
  greet() {
    return 'base';
  }
}
class Derived extends Base {
  greet() {
    return super.greet() + ':derived';
  }
}
function driveSuper(d) {
  return d.greet();
}
let superOk = true;
const d = new Derived();
for (let i = 0; i < HOT; i++) {
  if (driveSuper(d) !== 'base:derived') superOk = false;
}
test('super.method() dispatch unaffected by devirtualization', superOk, true);

console.log('\narrow methods bind lexical this\n');

function makeCounter(base) {
  return {
    base,
    bumped: () => base + 100
  };
}
function driveArrow(o) {
  return o.bumped();
}
let arrowAcc = 0;
for (let i = 0; i < HOT; i++) arrowAcc += driveArrow(makeCounter(i & 3));
let arrowRef = 0;
for (let i = 0; i < HOT; i++) arrowRef += (i & 3) + 100;
test('arrow-valued method uses lexical this', arrowAcc, arrowRef);

console.log('\nbound methods keep prepended arguments\n');

function addThree(a, b, c) {
  return a + b + c;
}
const holder = { f: addThree.bind(null, 10, 20) };
function driveBound(o) {
  return o.f(5);
}
let boundAcc = 0;
for (let i = 0; i < HOT; i++) boundAcc += driveBound(holder);
test('bound method with prepended args', boundAcc, HOT * (10 + 20 + 5));

console.log('\nthrowing methods propagate\n');

class Boom {
  maybe(n) {
    if (n === 42) throw new Error('boom');
    return n;
  }
}
function driveThrow(o, n) {
  return o.maybe(n);
}
const boom = new Boom();
let threwAt42 = false;
let normalOk = true;
for (let i = 0; i < HOT; i++) {
  const n = i + 1000;
  if (driveThrow(boom, n) !== n) normalOk = false;
}
try {
  driveThrow(boom, 42);
} catch (e) {
  threwAt42 = e.message === 'boom';
}
test('inlined method returns value on the hot path', normalOk, true);
test('inlined method still throws when it should', threwAt42, true);

summary();
