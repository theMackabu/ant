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

console.log('test_jit_builtin_direct_call: ok');
