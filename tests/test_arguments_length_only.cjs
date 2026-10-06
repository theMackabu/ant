// functions whose only use of `arguments` is reading `arguments.length` skip
// the arguments object; every other shape must keep full semantics

function assert(condition, message) {
  if (!condition) throw new Error(message);
}

function eq(actual, expected, label) {
  assert(actual === expected, `${label}: expected ${String(expected)}, got ${String(actual)}`);
}

function plain(x, y) { return arguments.length; }
function strict() { 'use strict'; return arguments.length; }
function reassigned(x) { x = 5; return arguments.length + ':' + x; }
function condition() { if (arguments.length > 1) return 'many'; return 'few'; }
function loop() { let n = 0; for (let i = 0; i < arguments.length; i++) n++; return n; }
function defaultParam(p = arguments.length) { return p; }
function nested() { return (function () { return arguments.length; })(1, 2, 3) + arguments.length; }

eq(plain(), 0, 'no args');
eq(plain(1), 1, 'fewer than params');
eq(plain(1, 2, 3), 3, 'more than params');
eq(strict(1, 2), 2, 'strict');
eq(reassigned(1, 2), '2:5', 'param reassignment');
eq(condition(1), 'few', 'condition 1');
eq(condition(1, 2), 'many', 'condition 2');
eq(loop(1, 2, 3, 4), 4, 'loop bound');
eq(defaultParam(), 0, 'default param');
eq(defaultParam(undefined, 2), 2, 'default param with extra args');
eq(nested(1), 4, 'nested function has its own arguments');

// shapes that need the real object
function mixed() { return arguments.length > 1 ? arguments[1] : 'none'; }
function written() { arguments.length = 9; return arguments.length; }
function arrow() { const g = () => arguments.length; return g(); }
function shadowed() { var arguments = [1]; return arguments.length; }
function viaEval() { return eval('arguments.length'); }
function spread() { return [...arguments].length + arguments.length; }
function computed() { return arguments['length']; }
function optional() { return arguments?.length; }

eq(mixed(1), 'none', 'mixed index read 1');
eq(mixed(1, 'two'), 'two', 'mixed index read 2');
eq(written(), 9, 'length write');
eq(arrow(1, 2), 2, 'arrow capture');
eq(shadowed(1, 2, 3), 1, 'shadowing var');
eq(viaEval(1, 2), 2, 'direct eval');
eq(spread(1, 2), 4, 'spread');
eq(computed(1, 2), 2, 'computed length key');
eq(optional(1), 1, 'optional member');

// hot enough to reach the JIT and the inliner
let total = 0;
function caller(i) { return plain(i) + strict(i, i) + loop(i, i, i); }
for (let i = 0; i < 200000; i++) total += caller(i);
eq(total, 200000 * 6, 'jit and inlined arguments.length');

console.log('test_arguments_length_only: ok');
