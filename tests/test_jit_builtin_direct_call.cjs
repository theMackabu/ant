// JIT call sites invoke builtins directly; errors, exceptions and `this`
// handling must match the generic call path

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

function hot(fn, n) {
  let r;
  for (let i = 0; i < n; i++) r = fn(i);
  return r;
}

// native throwing inside a JIT frame, caught in the same frame
let caught = 0;
function thrower(i) {
  try { return JSON.parse(i % 1000 === 0 ? '{bad' : '1'); }
  catch (e) { caught++; return e instanceof SyntaxError; }
}
hot(thrower, 200000);
assert(caught === 200, `expected 200 caught parse errors, got ${caught}`);

// native error propagating out through JIT frames
function reduceEmpty() { return [].reduce((a, b) => a + b); }
let propagated = null;
try { hot(reduceEmpty, 10); } catch (e) { propagated = e; }
assert(propagated instanceof TypeError, 'reduce of empty array should throw TypeError');

// late throw after the call site has been hot for a while
function lateThrow(i) { return i === 150000 ? BigInt(1.5) : Math.abs(-i); }
let late = null;
try { hot(lateThrow, 200000); } catch (e) { late = e; }
assert(late instanceof Error, 'late native throw should propagate');

// builtin called as a plain function and through call()
const toStr = Object.prototype.toString;
function plainCall() { return toStr(); }
const plainTag = hot(plainCall, 100000);
assert(typeof plainTag === 'string' && plainTag.startsWith('[object '), `unexpected tag ${plainTag}`);

const push = Array.prototype.push;
function viaCall(i) { const a = []; push.call(a, i); return a.length; }
assert(hot(viaCall, 100000) === 1, 'push.call');

// builtin returning its receiver for chaining
const { EventEmitter } = require('events');
const ee = new EventEmitter();
function chain() { return ee.setMaxListeners(20).getMaxListeners(); }
assert(hot(chain, 100000) === 20, 'chained builtin calls');

// builtins called with many arguments
function maxOf(i) { return Math.max(i, 1, 2, 3, 4, 5, 6, 7); }
assert(hot(maxOf, 100000) === 99999, 'many-argument builtin call');

// sites compiled after calling only JS functions skip the direct builtin
// block; builtins reaching them later must still work, as must sites that
// mix both and sites that switch the other way
function viaJs(f, x) { return f(x); }
function viaBuiltin(f, x) { return f(x); }
function mixed(f, x) { return f(x); }
function viaMethod(o, x) { return o.m(x); }
const inc = x => x + 1;
let mixedSum = 0;
for (let i = 0; i < 5000; i++) {
  mixedSum += viaJs(inc, i) + viaBuiltin(Math.abs, -i) + mixed(i & 1 ? inc : Math.abs, -i);
  mixedSum += viaMethod(i & 1 ? { m: inc } : { m: Math.abs }, -i);
}
for (let i = 0; i < 5000; i++) mixedSum += viaJs(Math.abs, -i) + viaBuiltin(inc, i) + viaMethod({ m: String }, i).length;
assert(mixedSum === 50018890, 'call sites switching between JS functions and builtins');

// a site compiled JS-only records the first builtin that reaches it and
// recompiles once, so it ends up on the direct path
const { spawnSync } = require('node:child_process');
const lateEnv = { ...process.env, ANT_DEBUG: 'dump/vm:op-warn', NO_COLOR: '1' };
delete lateEnv.FORCE_COLOR;
const lateSite = spawnSync(process.execPath, ['-e', `
function site(f, x) { return f(x); }
const inc = x => x + 1;
for (let i = 0; i < 20000; i++) site(inc, i);
function loop(n) { let s = 0; for (let i = 0; i < n; i++) s += site(Math.abs, -i); return s; }
let total = 0;
for (let r = 0; r < 300; r++) total += loop(1000);
console.log(total);
`], { encoding: 'utf8', env: lateEnv, timeout: 30000 });
assert(lateSite.status === 0, lateSite.stderr);
assert(lateSite.stdout.trim() === String(300 * 499500), 'late builtin results');
if (typeof Ant !== 'undefined') {
  const loopCompiles = (lateSite.stderr.match(/^jit: compiled func=loop /gm) || []).length;
  assert(loopCompiles === 2, 'late builtin recompiled the caller once, got ' + loopCompiles);
}

// push/toString sites whose cache only saw user methods skip the inline
// builtin paths; real arrays and numbers reaching them later still work
class Queue { constructor() { this.n = 0; } push(x) { this.n += x; } toString() { return 'Q' + this.n; } }
const userQueue = new Queue();
function pushOne(target, x) { target.push(x); return target.length === undefined ? target.n : target.length; }
function show(v) { return v.toString(); }
for (let i = 0; i < 3000; i++) { pushOne(userQueue, 1); show(userQueue); }
const realArray = [];
assert(pushOne(realArray, 7) === 1 && pushOne(realArray, 8) === 2 && realArray.join() === '7,8', 'array reaching a user push site');
assert(show(255) === '255' && show(userQueue) === 'Q3000', 'number reaching a user toString site');

console.log('test_jit_builtin_direct_call: ok');
