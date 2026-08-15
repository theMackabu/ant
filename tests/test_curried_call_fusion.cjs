let failed = 0;
const eq = (n, a, e) => { const ok = Object.is(a, e); if (!ok) failed++; console.log(`${ok?'PASS':'FAIL'} ${n}${ok?'':` (got ${a}, want ${e})`}`); };

// basic curried step fusion (args are locals -> OP_CALL_CALL emitted)
const add = (a) => (b) => a + b;
{ const x = 1, y = 2; eq('curried add', add(x)(y), 3); }

// child mutates the captured param
const acc = (a) => (b) => { a = a + b; return a; };
{ const x = 10, y = 5; eq('captured mutation', acc(x)(y), 15); }

// captured parent upvalue passes through
function outer(k) { return (a) => (b) => a + b + k; }
{ const f = outer(100), x = 1, y = 2; eq('parent upvalue', f(x)(y), 103); }

// three-deep chain (compare-style)
const cmp = (o) => (x) => (y) => (x < y ? -1 : x > y ? 1 : 0) * o;
{ const o = 1, x = 3, y = 7; eq('three-deep chain', cmp(o)(x)(y), -1); }

// non-curried callee falls back generically
const twice = (f) => (v) => f(f(v));
const inc = (q) => q + 1;
{ const v = 5; eq('hof chain', twice(inc)(v), 7); }

// callee with side effects (not a curried step) keeps order
let log = '';
const noisy = (a) => { log += 'F'; return (b) => { log += 'S'; return a * b; } };
{ const x = 3, y = 4; eq('side-effect chain', noisy(x)(y), 12); eq('order', log, 'FS'); }

// literal outer args
eq('literal args', add(40)(2), 42);

// bound curried step must NOT fuse incorrectly
const badd = add.bind(null);
{ const x = 7, y = 8; eq('bound chain', badd(x)(y), 15); }

// intermediate that throws when called with wrong receiver semantics
const table = { mk: (a) => (b) => a - b };
{ const x = 9, y = 4; eq('member inner callee', table.mk(x)(y), 5); }

// error propagation from child body
const boom = (a) => (b) => { throw new Error('boom' + (a + b)); };
{ const x = 1, y = 2;
  let msg = '';
  try { boom(x)(y); } catch (e) { msg = e.message; }
  eq('child throw', msg, 'boom3');
}

// non-function intermediate -> TypeError
const notfn = (a) => a + 1;
{ const x = 1, y = 2;
  let ok = false;
  try { notfn(x)(y); } catch (e) { ok = e instanceof TypeError; }
  eq('non-function intermediate', ok, true);
}

// hot loop: exercise JIT tier of the fused site
function hot(n) {
  let s = 0;
  for (let i = 0; i < n; i++) { const a = i, b = i + 1; s += add(a)(b); }
  return s;
}
eq('hot fused loop', hot(100000), 10000000000);
// simpler exact check:
{ let s = 0; for (let i = 0; i < 1000; i++) { const a = i, b = 2 * i; s += add(a)(b); } eq('fused loop sum', s, 3 * (999 * 1000) / 2); }

// The inner call must run before a mutable outer argument is read.
function sideEffectBeforeOuterArg(initial) {
  let b = initial;
  function X(a) { b = 99; return y => y; }
  return X(0)(b);
}
eq('inner side effect before outer arg', sideEffectBeforeOuterArg(1), 99);
{ let sum = 0; for (let i = 0; i < 100000; i++) sum += sideEffectBeforeOuterArg(i); eq('inner side effect before outer arg hot', sum, 9900000); }

// Capture metadata is discovered lazily: this call site is compiled before
// the later closure captures b, but its outer read must still remain deferred.
function futureCaptureOrder() {
  let b = 1, mutate = null;
  function X(a) { if (mutate) mutate(); return y => y; }
  const first = X(0)(b);
  mutate = () => { b = 77; };
  return first + X(0)(b);
}
eq('future capture preserves call order', futureCaptureOrder(), 78);
{ let sum = 0; for (let i = 0; i < 100000; i++) sum += futureCaptureOrder(); eq('future capture preserves call order hot', sum, 7800000); }

// Cover each JIT slot representation used by the deferred-read opcode.
function capturedParamOrder(b) {
  function X(a) { b = 41; return y => y + 1; }
  return X(0)(b);
}
eq('captured parameter read is deferred', capturedParamOrder(1), 42);
{ let sum = 0; for (let i = 0; i < 10000; i++) sum += capturedParamOrder(i); eq('captured parameter read is deferred hot', sum, 420000); }

function plainParamFast(b) { return add(1)(b); }
{ let sum = 0; for (let i = 0; i < 10000; i++) sum += plainParamFast(i); eq('plain parameter slot hot', sum, 50005000); }

function writtenParamFast(b) { b += 1; return add(1)(b); }
{ let sum = 0; for (let i = 0; i < 10000; i++) sum += writtenParamFast(i); eq('written parameter slot hot', sum, 50015000); }

function numericLocalFast(n) {
  let sum = 0;
  for (let i = 0; i < n; i++) { let b = i + 1; sum += add(i)(b); }
  return sum;
}
eq('numeric local slot hot', numericLocalFast(10000), 100000000);

function deferredErrorCaught(b) {
  try { return notfn(1)(b); } catch (e) { return e instanceof TypeError ? b : -1; }
}
{ let sum = 0; for (let i = 0; i < 10000; i++) sum += deferredErrorCaught(i); eq('deferred slot error enters catch', sum, 49995000); }

const takeSecond = (a) => (b) => b;
function builderSlotFast() {
  let b = '';
  for (let i = 0; i < 200; i++) b += 'x';
  return takeSecond(0)(b).length;
}
eq('deferred builder slot fast path', builderSlotFast(), 200);

function builderSlotGeneric() {
  let b = '';
  for (let i = 0; i < 200; i++) b += 'x';
  function X(a) { b += 'z'; return y => y.length; }
  return X(0)(b);
}
eq('deferred builder slot generic path', builderSlotGeneric(), 201);
{ let sum = 0; for (let i = 0; i < 1000; i++) sum += builderSlotFast() + builderSlotGeneric(); eq('deferred builder slots hot', sum, 401000); }

// closure identity: intermediates must be distinct fresh closures when observed
const step = (a) => (b) => a + b;
const s1 = step(1);
const s2 = step(2);
eq('observed intermediates distinct', s1(10) + s2(10), 23);

// child with default param
const dflt = (a) => (b = 100) => a + b;
{ const x = 5; eq('default param child', dflt(x)(), 105); }

// child using rest
const rest = (a) => (...bs) => a + bs.length;
{ const x = 1, y = 2, z = 3; eq('rest child', rest(x)(y, z), 3); }

// name/toString of frames in errors still work through fused calls
const thrower = (a) => (b) => { return a.nope.deep; };
{ const x = null, y = 1;
  let caught = false;
  try { thrower(x)(y); } catch (e) { caught = e instanceof TypeError && typeof e.stack === 'string'; }
  eq('error stack through fusion', caught, true);
}

if (failed) { console.log(`${failed} FAILED`); process.exit(1); }
console.log('OK');
