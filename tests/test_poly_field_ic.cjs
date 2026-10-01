// Property reads at a site that sees several shapes or prototypes keep the
// other cases in a small polymorphic cache (sv_gf_poly_t), checked inline by
// compiled code. Every change that invalidates the single-case cache must
// invalidate those entries too: prototype method replacement, getters added
// later, __proto__ changes, deletes, and more classes than the cache holds.
const assert = require('node:assert');

function makeClasses(n) {
  const classes = [];
  for (let i = 0; i < n; i++) {
    const C = function (v) { this.v = v; if (i % 2) this.extra = i; };
    C.prototype.run = function () { return 'run' + i + ':' + this.v; };
    C.prototype.tag = i;
    classes.push(C);
  }
  return classes;
}

// one read site for methods, one for data on the prototype, one own field
function callRun(o) { return o.run(); }
function readTag(o) { return o.tag; }
function readV(o) { return o.v; }

function expected(o) {
  const run = Object.getPrototypeOf(o).run;
  return [run.call(o), o.tag, o.v];
}

function check(objs, rounds) {
  for (let r = 0; r < rounds; r++) {
    for (const o of objs) {
      const exp = expected(o);
      assert.strictEqual(callRun(o), exp[0]);
      assert.strictEqual(readTag(o), exp[1]);
      assert.strictEqual(readV(o), exp[2]);
    }
  }
}

const classes = makeClasses(4);
const objs = classes.map((C, i) => new C(i * 10));

// warm up until the functions are compiled with polymorphic caches
check(objs, 3000);

// replacing a method on one prototype
classes[2].prototype.run = function () { return 'replaced:' + this.v; };
check(objs, 200);

// a getter added later on a prototype, shadowing nothing before
Object.defineProperty(classes[1].prototype, 'tag', { get() { return 'getter' + this.v; }, configurable: true });
check(objs, 200);

// an own property that shadows the prototype's
objs[3].tag = 'own';
check(objs, 200);

// deleting it again
delete objs[3].tag;
check(objs, 200);

// changing an instance's prototype
Object.setPrototypeOf(objs[0], classes[3].prototype);
check(objs, 200);

// changing a prototype's prototype, so a holder deeper in the chain appears
const base = { run() { return 'base:' + this.v; }, tag: 'base' };
const C5 = function (v) { this.v = v; };
C5.prototype = Object.create(base);
const deep = new C5(5);
check([...objs, deep], 200);
base.run = function () { return 'base2:' + this.v; };
check([...objs, deep], 200);

// shadowing the deeper holder from the prototype in between: the receiver's
// shape and prototype stay the same, only the cache epoch says it changed
check([...objs, deep], 2000);
C5.prototype.run = function () { return 'mid:' + this.v; };
C5.prototype.tag = 'mid';
check([...objs, deep], 200);

// more classes than the cache has entries
const many = makeClasses(9).map((C, i) => new C(i));
check([...objs, ...many], 500);

// deleting a prototype method falls through to the next one in the chain
Object.setPrototypeOf(classes[0].prototype, { run() { return 'up:' + this.v; } });
delete classes[0].prototype.run;
check([...objs, ...many, new classes[0](7)], 200);

// a site compiled while it saw one shape, which only later sees others: the
// compiled code checks the cache's other cases at run time
function readLate(o) { return o.late; }
const L1 = function () { this.late = 1; };
for (let i = 0; i < 20000; i++) assert.strictEqual(readLate(new L1()), 1);
const lateProto = { late: 4 };
const lateObjs = [new L1(), { late: 2 }, { x: 0, late: 3 }, Object.create(lateProto), { y: 1, z: 2, late: 5 }];
const lateWant = [1, 2, 3, 4, 5];
for (let r = 0; r < 2000; r++)
  lateObjs.forEach((o, i) => assert.strictEqual(readLate(o), lateWant[i]));
lateProto.late = 40;
lateWant[3] = 40;
lateObjs[1].late = 20;
lateWant[1] = 20;
for (let r = 0; r < 200; r++)
  lateObjs.forEach((o, i) => assert.strictEqual(readLate(o), lateWant[i]));
Object.defineProperty(lateProto, 'late', { get() { return 'got'; } });
lateWant[3] = 'got';
for (let r = 0; r < 200; r++)
  lateObjs.forEach((o, i) => assert.strictEqual(readLate(o), lateWant[i]));

// A megamorphic site: one constructor wrapper shared by many classes, whose
// fresh `this` objects share a shape and differ only by prototype
// (Prototype.js-style Class.create). It goes through the shared read cache,
// and compiled code probes that cache after a recompile.
function createClass() {
  return function () { this.initialize.apply(this, arguments); };
}
const classesMega = [];
for (let k = 0; k < 12; k++) {
  const K = createClass();
  K.prototype = { initialize(v) { this.v = v * 100 + k; }, kind: k };
  classesMega.push(K);
}
function makeMega(i) { return new classesMega[i % 12](i); }
for (let i = 0; i < 60000; i++) assert.strictEqual(makeMega(i).v, i * 100 + (i % 12));

// replacing one class's initialize after the site went megamorphic
classesMega[5].prototype.initialize = function (v) { this.v = -v; };
for (let i = 0; i < 6000; i++)
  assert.strictEqual(makeMega(i).v, i % 12 === 5 ? -i : i * 100 + (i % 12));

// a getter shadowing initialize on another class's prototype chain
Object.setPrototypeOf(classesMega[7].prototype, {
  get initialize() { return function (v) { this.v = 'g' + v; }; },
});
delete classesMega[7].prototype.initialize;
for (let i = 0; i < 6000; i++) {
  const want = i % 12 === 5 ? -i : i % 12 === 7 ? 'g' + i : i * 100 + (i % 12);
  assert.strictEqual(makeMega(i).v, want);
}

// A megamorphic site reading from a grandparent prototype: shadowing it on
// the prototype in between changes neither the receiver's shape nor its
// prototype, only the cache epoch says the cached holder is stale.
const grand = { greet() { return 'grand'; } };
const megaKinds = [];
for (let k = 0; k < 14; k++) {
  const mid = Object.create(grand);
  const make = function () { this['k' + (k % 3)] = k; };
  make.prototype = mid;
  megaKinds.push({ make, mid });
}
function greetOf(o) { return o.greet(); }
const megaObjs = megaKinds.map(({ make }) => new make());
for (let r = 0; r < 4000; r++) megaObjs.forEach(o => assert.strictEqual(greetOf(o), 'grand'));
megaKinds[3].mid.greet = function () { return 'mid3'; };
for (let r = 0; r < 500; r++)
  megaObjs.forEach((o, i) => assert.strictEqual(greetOf(o), i === 3 ? 'mid3' : 'grand'));

console.log('PASS polymorphic property reads follow prototype and shape changes');
