const assert = require('node:assert');

function top(stack) {
  return stack.split('\n')[1].trim().split(' (')[0];
}

function Plain() { this.stack = new Error('plain').stack; }
function plainCall() { return new Error('call').stack; }
class Base { constructor() { this.stack = new Error('base').stack; } }
class Derived extends Base { constructor() { super(); } }
class FromPlain extends Plain { constructor() { super(); } }
const arrow = () => new Error('arrow').stack;
const bound = Plain.bind(null);

function viaNew() { return new Plain().stack; }
function viaReflect() { return Reflect.construct(Plain, []).stack; }
function viaReflectTarget() { return Reflect.construct(Plain, [], Base).stack; }
function viaBound() { return new bound().stack; }
function viaApply() { return Reflect.apply(plainCall, undefined, []); }
function viaCall() { return plainCall.call(undefined); }
function viaMap() { return [0].map(plainCall)[0]; }
function viaDerived() { return new Derived().stack; }
function viaReflectClass() { return Reflect.construct(Base, []).stack; }
function viaFromPlain() { return new FromPlain().stack; }
function viaArrow() { return arrow(); }

function check() {
  assert.strictEqual(top(viaNew()), 'at new Plain');
  assert.strictEqual(top(viaReflect()), 'at new Plain');
  assert.strictEqual(top(viaReflectTarget()), 'at new Plain');
  assert.strictEqual(top(viaBound()), 'at new Plain');
  assert.strictEqual(top(viaApply()), 'at plainCall');
  assert.strictEqual(top(viaCall()), 'at plainCall');
  assert.strictEqual(top(viaMap()), 'at plainCall');
  assert.strictEqual(top(viaDerived()), 'at new Base');
  assert.strictEqual(top(viaReflectClass()), 'at new Base');
  assert.strictEqual(top(viaFromPlain()), 'at new Plain');
  assert.strictEqual(top(viaArrow()), 'at arrow');
}

for (const limit of [10, 2, 1]) {
  Error.stackTraceLimit = limit;
  for (let i = 0; i < 3000; i++) check();
}

Error.stackTraceLimit = 2;
function second(stack) {
  return stack.split('\n')[2].trim().split(' (')[0];
}
for (let i = 0; i < 3000; i++) {
  assert.strictEqual(second(viaDerived()), 'at new Derived');
  assert.strictEqual(second(viaFromPlain()), 'at new FromPlain');
  assert.strictEqual(second(viaNew()), 'at viaNew');
}

Error.stackTraceLimit = 10;
function Outer() { this.inner = Reflect.construct(Plain, []); this.called = plainCall(); }
function nested() { return new Outer(); }
for (let i = 0; i < 3000; i++) {
  const made = nested();
  const lines = made.inner.stack.split('\n').map(line => line.trim().split(' (')[0]);
  assert.strictEqual(lines[1], 'at new Plain');
  assert.ok(lines.includes('at new Outer'));
  assert.ok(lines.includes('at nested'));
  const called = made.called.split('\n').map(line => line.trim().split(' (')[0]);
  assert.strictEqual(called[1], 'at plainCall');
  assert.strictEqual(called[2], 'at new Outer');
}

console.log('ok');
