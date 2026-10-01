// Named stores at a site that sees several receiver shapes keep the other
// cases (sv_pf_poly_t): stores to existing properties and adds that move a
// shape to the next. Every store must still respect read-only properties,
// inherited setters, frozen, sealed and non-extensible objects, and cases
// must not outlive the shapes they were made for.
const assert = require('node:assert');

function setLeft(o, v) { o.left = v; }

// existing properties at different slots in different shapes
const shapes = [
  () => ({ left: 0 }),
  () => ({ a: 1, left: 0 }),
  () => ({ a: 1, b: 2, left: 0 }),
  () => ({ right: 1, left: 0 }),
  () => ({ x: 1, y: 2, z: 3, w: 4, left: 0 }), // left in overflow storage
];
for (let r = 0; r < 5000; r++) {
  for (const make of shapes) {
    const o = make();
    setLeft(o, r);
    assert.strictEqual(o.left, r);
  }
}

// adds from several starting shapes, like splay tree nodes that inherit
// left/right from their prototype until they get their own
function Node(k) { this.key = k; }
Node.prototype.left = null;
Node.prototype.right = null;
function link(parent, child, side) { if (side) parent.right = child; else parent.left = child; }
for (let r = 0; r < 3000; r++) {
  const a = new Node(r), b = new Node(r + 1), c = new Node(r + 2);
  link(a, b, r & 1);
  link(a, c, (r + 1) & 1);
  link(b, c, 0);
  assert.strictEqual(a.left.key + a.right.key, 2 * r + 3);
  assert.strictEqual(b.left, c);
  assert.strictEqual(c.left, null);
  assert.ok(Object.prototype.hasOwnProperty.call(a, 'left'));
}

// a read-only left on one shape: the store is ignored (sloppy mode)
const ro = { left: 7 };
Object.defineProperty(ro, 'left', { writable: false });
for (let r = 0; r < 2000; r++) {
  setLeft(ro, r);
  assert.strictEqual(ro.left, 7);
  const o = shapes[r % shapes.length]();
  setLeft(o, r);
  assert.strictEqual(o.left, r);
}

// an inherited setter on one class
let seen = 0;
function WithSetter() {}
Object.defineProperty(WithSetter.prototype, 'left', { set(v) { seen = v; }, get() { return 'getter'; } });
for (let r = 0; r < 2000; r++) {
  const w = new WithSetter();
  setLeft(w, r);
  assert.strictEqual(seen, r);
  assert.strictEqual(w.left, 'getter');
  assert.ok(!Object.prototype.hasOwnProperty.call(w, 'left'));
}

// frozen, sealed and non-extensible objects of shapes the site has seen
for (let r = 0; r < 500; r++) {
  const f = Object.freeze({ left: 1 });
  setLeft(f, r);
  assert.strictEqual(f.left, 1);
  const s = Object.seal({ left: 1 });
  setLeft(s, r);
  assert.strictEqual(s.left, r);
  const n = Object.preventExtensions(new Node(r));
  setLeft(n, r);
  assert.ok(!Object.prototype.hasOwnProperty.call(n, 'left'));
  assert.strictEqual(n.left, null);
}

// more shapes than the site's cache holds
const many = [];
for (let k = 0; k < 20; k++) {
  const o = {};
  for (let j = 0; j < k % 7; j++) o['p' + k + '_' + j] = j;
  o['q' + k] = k;
  o.left = 0;
  many.push(o);
}
for (let r = 0; r < 2000; r++) many.forEach((o, i) => { setLeft(o, r + i); assert.strictEqual(o.left, r + i); });

// An add depends on the receiver's prototype chain, which isn't part of the
// shape: an inherited setter runs instead of adding, and an inherited
// read-only property rejects the add (throws in strict code). Objects of the
// same shape under different prototypes must not share an add case.
{
  let setterCalls = 0;
  class WithSetter { constructor() { this.a = 1; } set y(v) { setterCalls++; } }
  class PlainY { constructor() { this.a = 1; } }
  class OtherY { constructor() { this.b = 1; this.c = 2; } }
  class ReadOnlyY { constructor() { this.a = 1; } }
  Object.defineProperty(ReadOnlyY.prototype, 'y', { value: 0, writable: false });
  const setY = new Function('o', "'use strict'; o.y = 2;");
  let readOnlyThrows = 0;
  for (let r = 0; r < 5000; r++) {
    const p = new PlainY();
    setY(p);
    assert.strictEqual(p.y, 2);
    setY(new OtherY());
    const w = new WithSetter();
    setY(w);
    assert.ok(!Object.prototype.hasOwnProperty.call(w, 'y'));
    try { setY(new ReadOnlyY()); } catch (e) { readOnlyThrows++; }
  }
  assert.strictEqual(setterCalls, 5000);
  assert.strictEqual(readOnlyThrows, 5000);

  // just two classes alternating at one add site
  let calls2 = 0;
  class A2 { constructor() { this.a = 1; } }
  class B2 { constructor() { this.a = 1; } set z(v) { calls2++; } }
  function setZ(o) { o.z = 1; }
  for (let r = 0; r < 5000; r++) { setZ(new A2()); setZ(new B2()); }
  assert.strictEqual(calls2, 5000);

  // objects from Object.create share a shape across prototypes: a site
  // compiled while it only added to plain objects must not reuse that add
  // for an object whose prototype has a setter or a read-only property
  let calls3 = 0, throws3 = 0;
  const withSetter = { set z(v) { calls3++; } };
  const readOnlyZ = Object.defineProperty({}, 'z', { value: 0, writable: false });
  const make = p => { const o = Object.create(p); o.a = 1; return o; };
  const addZ = new Function('o', "'use strict'; o.z = 1;");
  for (let r = 0; r < 20000; r++) addZ(make(Object.prototype));
  for (let r = 0; r < 5000; r++) {
    const o = make(withSetter);
    addZ(o);
    assert.ok(!Object.prototype.hasOwnProperty.call(o, 'z'));
    try { addZ(make(readOnlyZ)); } catch (e) { throws3++; }
  }
  assert.strictEqual(calls3, 5000);
  assert.strictEqual(throws3, 5000);

  // a prototype that gains a setter, then a read-only property, after the
  // site cached adds under it
  let calls4 = 0, throws4 = 0;
  const later = {};
  const makeLater = () => { const o = Object.create(later); o.a = 1; return o; };
  const addW = new Function('o', "'use strict'; o.w = 1;");
  for (let r = 0; r < 20000; r++) assert.strictEqual(addW(makeLater()), undefined);
  Object.defineProperty(later, 'w', { set(v) { calls4++; }, configurable: true });
  for (let r = 0; r < 5000; r++) {
    const o = makeLater();
    addW(o);
    assert.ok(!Object.prototype.hasOwnProperty.call(o, 'w'));
  }
  Object.defineProperty(later, 'w', { value: 0, writable: false });
  for (let r = 0; r < 5000; r++) { try { addW(makeLater()); } catch (e) { throws4++; } }
  assert.strictEqual(calls4, 5000);
  assert.strictEqual(throws4, 5000);
}

console.log('PASS polymorphic property stores respect shapes and property rules');
