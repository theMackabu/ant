// Compiled `new f(...)` calls a compiled constructor directly
// (jit_emit_new_direct): jit_helper_new_this allocates `this`, the
// constructor's compiled code runs, and the result is `this` unless the
// constructor returned an object. Everything that doesn't qualify takes
// jit_helper_new. Both routes must give the same results.
const assert = require('node:assert');

function Plain(a, b) { this.a = a; this.b = b; }
Plain.prototype.sum = function () { return this.a + this.b; };

function ReturnsObject(v) { this.ignored = v; return { replaced: v }; }
function ReturnsPrimitive(v) { this.kept = v; return 42; }
function ReturnsFunction(v) { this.x = v; return function inner() { return v; }; }
function Throws(v) { this.before = v; if (v % 3 === 0) throw new Error('boom ' + v); this.after = v; }
function Target() { this.same = new.target === Target; }
function Defaults(a, b = a * 2) { this.a = a; this.b = b; }
function Arity() { this.n = arguments.length; this.last = arguments[arguments.length - 1]; }

function makeCounter(start) {
  let count = start;
  function Counter() { count++; this.value = count; }
  return { Counter, get: () => count };
}

class Base { constructor(v) { this.v = v; } }
class Derived extends Base { constructor(v) { super(v); this.d = v * 2; } }

const bound = Plain.bind(null, 100);
const proxied = new Proxy(Plain, {});

function Deep(n) { this.n = n; this.next = n > 0 ? new Deep(n - 1) : null; }

function construct(i) {
  const p = new Plain(i, i + 1);
  assert.strictEqual(p.sum(), 2 * i + 1);
  assert.ok(p instanceof Plain);

  assert.deepStrictEqual(new ReturnsObject(i), { replaced: i });
  const prim = new ReturnsPrimitive(i);
  assert.strictEqual(prim.kept, i);
  assert.ok(prim instanceof ReturnsPrimitive);
  assert.strictEqual(new ReturnsFunction(i)(), i);

  let threw = false;
  try { assert.strictEqual(new Throws(i).after, i); } catch (e) { threw = true; assert.strictEqual(e.message, 'boom ' + i); }
  assert.strictEqual(threw, i % 3 === 0);

  assert.strictEqual(new Target().same, true);
  const d = new Defaults(i);
  assert.strictEqual(d.b, 2 * i);
  const ar = new Arity(1, 2, i);
  assert.strictEqual(ar.n, 3);
  assert.strictEqual(ar.last, i);

  const der = new Derived(i);
  assert.strictEqual(der.v + der.d, 3 * i);
  assert.ok(der instanceof Base);

  const b = new bound(i);
  assert.strictEqual(b.a + b.b, 100 + i);
  assert.ok(b instanceof Plain);
  const px = new proxied(i, 1);
  assert.strictEqual(px.sum(), i + 1);
}

for (let i = 0; i < 20000; i++) construct(i);

// captured state is shared between the direct call and the closure
const { Counter, get } = makeCounter(10);
let last;
for (let i = 0; i < 5000; i++) last = new Counter();
assert.strictEqual(last.value, 5010);
assert.strictEqual(get(), 5010);

// a constructor first seen after the caller was compiled
function Late(v) { this.late = v; }
function build(C, v) { return new C(v); }
for (let i = 0; i < 20000; i++) build(Plain, i);
for (let i = 0; i < 2000; i++) assert.strictEqual(build(Late, i).late, i);

// replacing the prototype after warm-up
Plain.prototype = { sum() { return 'new' + this.a; } };
for (let i = 0; i < 1000; i++) assert.strictEqual(new Plain(i, 0).sum(), 'new' + i);

// recursion through the direct path
assert.strictEqual(new Deep(200).next.next.n, 198);
let depthError = null;
try { new Deep(1e6); } catch (e) { depthError = e; }
assert.ok(depthError instanceof RangeError, 'deep recursion reports a RangeError');

// a function over the direct-new size cap (JIT_NEW_DIRECT_MAX_CODE_LEN)
// calls the helper; results and prototypes must be the same
class Big { constructor(v) { this.v = v; } }
const bigBody = [];
for (let k = 0; k < 200; k++) bigBody.push(`s += new Big(i + ${k}).v;`);
const bigNew = new Function('Big', 'i', `let s = 0; ${bigBody.join(' ')} return s;`);
for (let i = 0; i < 3000; i++) assert.strictEqual(bigNew(Big, i), 200 * i + 19900);
const lastBig = new Function('Big', 'return new Big(7);')(Big);
assert.ok(lastBig instanceof Big && lastBig.v === 7);

console.log('PASS compiled new calls constructors directly');
