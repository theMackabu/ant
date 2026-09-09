'use strict';
function same(a, b, name) {
  if (!Object.is(a, b)) throw new Error(name + ': ' + a + ' != ' + b);
}
const apply = Function.prototype.apply;
function forward() { return this.invoke.apply(this, arguments); }
function Construct() { this.initialize.apply(this, arguments); }
const receiver = { invoke(a, b, c) { return [this, a, b, c, arguments.length]; } };
for (let i = 0; i < 1000; i++) {
  const result = forward.call(receiver, i, -0, 'value');
  same(result[0], receiver, 'receiver');
  same(result[1], i, 'argument');
  same(result[2], -0, 'negative zero');
  same(result[3], 'value', 'string');
  same(result[4], 3, 'arity');
}
same(forward.call(receiver)[4], 0, 'empty arguments');
same(forward.call(receiver, 1, 2, 3, 4, 5, 6)[4], 6, 'extra arguments');
let seen;
receiver.invoke.apply = function (self, args) {
  same(this, receiver.invoke, 'custom apply receiver');
  same(self, receiver, 'custom apply this argument');
  seen = args;
  return args[0];
};
same(forward.call(receiver, 42), 42, 'overridden apply');
same(seen.length, 1, 'materialized length');
same(seen[0], 42, 'materialized value');
let calleeThrows = false;
try { seen.callee; } catch (error) { calleeThrows = error instanceof TypeError; }
same(calleeThrows, true, 'strict arguments callee');
const saved = seen;
same(forward.call(receiver, 43), 43, 'second materialization');
same(seen === saved, false, 'per-call arguments identity');
same(saved[0], 42, 'escaped arguments remain alive');
delete receiver.invoke.apply;
same(forward.call(receiver, 44)[1], 44, 'restore builtin apply');
let events = [];
const target = function (value) { events.push('call'); return value; };
Object.defineProperty(target, 'apply', { configurable: true, get() { events.push('apply'); return apply; } });
const getters = { get invoke() { events.push('target'); return target; } };
for (let i = 0; i < 600; i++) {
  events = [];
  same(forward.call(getters, i), i, 'getter target');
  same(events.join(','), 'target,apply,call', 'lookup order');
}
Object.defineProperty(target, 'apply', { get() { throw new Error('apply getter'); } });
let thrown = '';
try { forward.call(getters, 1); } catch (error) { thrown = error.message; }
same(thrown, 'apply getter', 'getter exception');
const bound = { invoke: receiver.invoke.bind(receiver, 7) };
same(forward.call(bound, 8)[1], 7, 'bound argument');
same(forward.call(bound, 8)[2], 8, 'forwarded bound argument');
const native = { invoke: Math.max };
same(forward.call(native, 1, 9, 3), 9, 'native target');
Construct.prototype.initialize = function (x, y) {
  this.x = x;
  this.y = y;
  this.target = new.target;
  return { ignored: true };
};
for (let i = 0; i < 1000; i++) {
  const value = new Construct(i, -0);
  same(value.x, i, 'constructor argument');
  same(value.y, -0, 'constructor negative zero');
  same(value.target, undefined, 'initializer is ordinary call');
  same(Object.getPrototypeOf(value), Construct.prototype, 'constructor prototype');
}
// These uses must retain normal arguments behavior rather than forwarding.
function changed() { arguments[0] = 12; return this.invoke.apply(this, arguments); }
function escaped() { this.saved = arguments; return this.invoke.apply(this, arguments); }
for (let i = 0; i < 600; i++) {
  same(changed.call(receiver, i)[1], 12, 'mutated arguments');
  same(escaped.call(receiver, i)[1], i, 'escaped arguments');
  same(receiver.saved[0], i, 'saved arguments');
}
Function.prototype.apply = function (self, args) {
  return Reflect.apply(apply, this, [self, args]);
};
try {
  for (let i = 0; i < 600; i++) same(forward.call(receiver, i)[1], i, 'prototype apply replacement');
} finally { Function.prototype.apply = apply; }
same(forward.call(receiver, 99)[1], 99, 'prototype apply restored');
let effects = 0;
const changing = { invoke(value) { effects++; this.value = value + 1; return this.value; } };
for (let i = 0; i < 600; i++) same(forward.call(changing, i), i + 1, 'warm direct initializer');
const before = effects;
same(forward.call(changing, 'x'), 'x1', 'direct initializer type change');
same(effects, before + 1, 'direct initializer effect once');
const recursive = { invoke(depth) { return depth ? forward.call(this, depth - 1) + 1 : 0; } };
for (let i = 0; i < 600; i++) same(forward.call(recursive, 20), 20, 'recursive forwarding');
console.log('PASS guarded argument forwarding');
