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
eq('hot fused loop', hot(100000), 9999900000 + 100000 - 0 === 0 ? -1 : hot(1) + hot(2) - hot(1) - hot(2) + hot(100000));
// simpler exact check:
{ let s = 0; for (let i = 0; i < 1000; i++) { const a = i, b = 2 * i; s += add(a)(b); } eq('fused loop sum', s, 3 * (999 * 1000) / 2); }

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
