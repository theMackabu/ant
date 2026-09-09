'use strict';
function same(a, b, name) { if (!Object.is(a, b)) throw new Error(name + ': ' + a + ' != ' + b); }
function make(C, value) { return new C(value); }
function Plain(value) { this.value = value; this.target = new.target; }
function Primitive(value) { this.value = value; return 17; }
function Replacement(value) { return { value, replacement: true }; }
for (let i = 0; i < 1000; i++) {
  const result = make(Plain, i);
  same(result.value, i, 'ordinary fields');
  same(result.target, Plain, 'new.target');
  same(Object.getPrototypeOf(result), Plain.prototype, 'ordinary prototype');
  same(make(Primitive, i).value, i, 'primitive return');
  same(make(Replacement, i).replacement, true, 'object return');
}
const oldProto = Plain.prototype;
Plain.prototype = { changed: true };
same(Object.getPrototypeOf(make(Plain, 1)), Plain.prototype, 'prototype replacement');
Plain.prototype = 3;
same(Object.getPrototypeOf(make(Plain, 1)), Object.prototype, 'primitive prototype fallback');
Plain.prototype = oldProto;
const Bound = Plain.bind({ ignored: true }, 41);
same(make(Bound, 1).value, 41, 'bound constructor argument');
same(make(Bound, 1).target, Plain, 'bound new.target');
let traps = 0;
const Proxied = new Proxy(Plain, { construct(target, args, newTarget) {
  traps++;
  same(newTarget, Proxied, 'proxy new.target');
  return { value: args[0] + 1 };
} });
same(make(Proxied, 2).value, 3, 'proxy construct');
same(traps, 1, 'single construct trap');
class Base { constructor(value) { this.value = value; this.target = new.target; } }
class Derived extends Base { constructor(value) { super(value + 1); this.extra = true; } }
for (let i = 0; i < 600; i++) {
  const value = make(Derived, i);
  same(value.value, i + 1, 'derived super');
  same(value.extra, true, 'derived this');
  same(value.target, Derived, 'derived new.target');
}
function Throwing(value) { if (value < 0) throw new Error('constructor throw'); this.value = value; }
for (let i = 0; i < 600; i++) same(make(Throwing, i).value, i, 'warm throwing constructor');
let message;
try { make(Throwing, -1); } catch (error) { message = error.message; }
same(message, 'constructor throw', 'constructor exception');
let rejected = false;
try { make(() => ({}), 1); } catch (error) { rejected = error instanceof TypeError; }
same(rejected, true, 'non-constructor rejection');
let constructions = 0;
function Changing(value) { this.sequence = ++constructions; this.value = value + 1; }
for (let i = 0; i < 600; i++) same(make(Changing, i).value, i + 1, 'warm arithmetic constructor');
const previous = constructions;
const changed = make(Changing, 'x');
same(changed.value, 'x1', 'constructor type change');
same(changed.sequence, previous + 1, 'constructor effect once');
same(constructions, previous + 1, 'no constructor replay');
console.log('PASS constructor dispatch');
