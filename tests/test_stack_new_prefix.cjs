const assert = require('node:assert');

const WARM = 600;
const NAMES = new Set(['Plain', 'Klass', 'Parent', 'Child', 'GrandChild', 'PlainParent', 'PlainChild', 'Deep', 'helper', 'thrower', 'Fielded',
  'FieldChild', 'Rest', 'RestChild', 'Args', 'Mid', 'Tail', 'Getter', 'Rec', 'executor', 'visit', 'Walker', 'Both', 'Built', 'Hop', 'Mixed', 'SpreadChild', 'SpreadPlainChild']);

function frames(error) {
  const out = [];
  for (const line of String(error.stack).split('\n')) {
    const m = /^\s+at (new )?([\w$.<>]+)(?: \[as \w+\])? \(/.exec(line);
    if (!m) continue;
    const name = m[2].split('.').pop();
    if (NAMES.has(name)) out.push((m[1] || '') + name);
  }
  return out.join(' < ');
}

let armed = false;
function thrower() { if (armed) throw new Error('x'); }

function Plain(a) { this.a = a; if (armed) throw new Error('x'); }
class Klass { constructor(a) { this.a = a; if (armed) throw new Error('x'); } }
class Parent { constructor(a) { this.a = a; if (armed) throw new Error('x'); } }
class Child extends Parent { constructor(a) { super(a); this.c = 1; } }
class GrandChild extends Child { constructor(a) { super(a); this.g = 1; } }
function PlainParent(a) { this.a = a; if (armed) throw new Error('x'); }
class PlainChild extends PlainParent { constructor(a) { super(a); this.c = 1; } }
function helper() { thrower(); }
function Deep(a) { this.a = a; helper(); }
class Fielded { f = 1; constructor(a) { this.a = a; if (armed) throw new Error('x'); } }
class FieldChild extends Fielded { constructor(a) { super(a); this.c = 1; } }
class Rest { constructor(...all) { this.all = all; if (armed) throw new Error('x'); } }
class RestChild extends Rest { constructor(a) { super(a, a); this.c = 1; } }
function Args(a) { this.n = arguments.length; this.a = a; if (armed) throw new Error('x'); }
function Mid(a) { this.p = new Plain(a); }
function Tail(a) { this.a = a; helper(); this.b = a; }
class Getter { constructor(a) { this.a = a; this.v = this.value; } get value() { thrower(); return 1; } }
function Walker(list) { this.total = 0; list.forEach(function visit(v) { this.total += v; thrower(); }, this); }
function Both(a) { this.a = a; this.made = new.target !== undefined; if (armed) throw new Error('x'); }
const BoundPlain = Plain.bind(null);
const one = [1];

const cases = {
  'call': [(a) => Plain.call({}, a), 'Plain'],
  'apply': [(a) => Plain.apply({}, [a]), 'Plain'],
  'Reflect.apply': [(a) => Reflect.apply(Plain, {}, [a]), 'Plain'],
  'new function': [(a) => new Plain(a), 'new Plain'],
  'new class': [(a) => new Klass(a), 'new Klass'],
  'super into class': [(a) => new Child(a), 'new Parent < new Child'],
  'super twice': [(a) => new GrandChild(a), 'new Parent < new Child < new GrandChild'],
  'super into function': [(a) => new PlainChild(a), 'new PlainParent < new PlainChild'],
  'Reflect.construct function': [(a) => Reflect.construct(Plain, [a]), 'new Plain'],
  'Reflect.construct class': [(a) => Reflect.construct(Klass, [a]), 'new Klass'],
  'Reflect.construct newTarget': [(a) => Reflect.construct(Parent, [a], Child), 'new Parent'],
  'Reflect.construct function newTarget': [(a) => Reflect.construct(Plain, [a], Klass), 'new Plain'],
  'callee below a constructor': [(a) => new Deep(a), 'thrower < helper < new Deep'],
  'parent with fields': [(a) => new FieldChild(a), 'new Fielded < new FieldChild'],
  'parent with rest': [(a) => new RestChild(a), 'new Rest < new RestChild'],
  'constructor using arguments': [(a) => new Args(a), 'new Args'],
  'constructor inside constructor': [(a) => new Mid(a), 'new Plain < new Mid'],
  'bound constructor': [(a) => new BoundPlain(a), 'new Plain'],
  'call in the middle of a constructor': [(a) => new Tail(a), 'thrower < helper < new Tail'],
  'getter inside constructor': [(a) => new Getter(a), 'thrower < new Getter'],
  'callback inside constructor': [() => new Walker(one), 'thrower < visit < new Walker'],
  'array callback': [(a) => one.map(function visit(v) { return new Plain(v); }), 'new Plain < visit'],
  'callback that is a constructor': [(a) => one.map(Plain, {}), 'Plain'],
};

function capture(fn) {
  armed = true;
  try {
    fn(1);
    return 'no throw';
  } catch (error) {
    return frames(error);
  } finally {
    armed = false;
  }
}

for (const [name, [fn, expected]] of Object.entries(cases)) {
  assert.strictEqual(capture(fn), expected, `${name} cold`);
  for (let i = 0; i < WARM; i++) fn(i);
  assert.strictEqual(capture(fn), expected, `${name} warm`);
  assert.strictEqual(capture(fn), expected, `${name} warm again`);
}

{
  let stack = '';
  const make = () => new Promise(function executor() { if (armed) stack = frames(new Error('x')); });
  for (let round = 0; round < 2; round++) {
    armed = true;
    make();
    armed = false;
    assert.strictEqual(stack, 'executor', `promise executor round ${round}`);
    for (let i = 0; i < WARM; i++) make();
  }
}

{
  const both = (a, construct) => construct ? new Both(a) : Both.call({}, a);
  for (let round = 0; round < 3; round++) {
    armed = true;
    let called, made;
    try { both(1, false); } catch (error) { called = frames(error); }
    try { both(1, true); } catch (error) { made = frames(error); }
    armed = false;
    assert.strictEqual(called, 'Both', `alternating call round ${round}`);
    assert.strictEqual(made, 'new Both', `alternating construct round ${round}`);
    for (let i = 0; i < WARM; i++) { both(i, false); both(i, true); }
  }
}

{
  function Rec(n) { this.n = n; if (n > 0) this.next = new Rec(n - 1); else thrower(); }
  const limit = Error.stackTraceLimit;
  const expected = ['thrower'];
  for (let i = 0; i <= 40; i++) expected.push('new Rec');
  for (let round = 0; round < 3; round++) {
    Error.stackTraceLimit = 100;
    armed = true;
    let seen;
    try { new Rec(40); } catch (error) { seen = frames(error); }
    armed = false;
    Error.stackTraceLimit = limit;
    assert.strictEqual(seen, expected.join(' < '), `recursive constructor round ${round}`);
    for (let i = 0; i < WARM; i++) new Rec(3);
  }
}

{
  const limit = Error.stackTraceLimit;
  const short = {
    'limit one, new function': [(a) => new Plain(a), 'new Plain'],
    'limit one, call': [(a) => Plain.call({}, a), 'Plain'],
    'limit one, super': [(a) => new PlainChild(a), 'new PlainParent'],
    'limit one, Reflect.construct': [(a) => Reflect.construct(Plain, [a]), 'new Plain'],
    'limit one, nested': [(a) => new Mid(a), 'new Plain'],
  };
  for (const [name, [fn, expected]] of Object.entries(short)) {
    for (let i = 0; i < WARM; i++) fn(i);
    Error.stackTraceLimit = 1;
    let seen;
    try { seen = capture(fn); } finally { Error.stackTraceLimit = limit; }
    assert.strictEqual(seen, expected, name);
  }
  Error.stackTraceLimit = 2;
  let two;
  try { two = capture((a) => new Mid(a)); } finally { Error.stackTraceLimit = limit; }
  assert.strictEqual(two, 'new Plain < new Mid', 'limit two, nested');
}

{
  const DEPTH = 60;
  const limit = Error.stackTraceLimit;
  let stack;
  function Pe(n) {
    if (n > 0) new Promise(function executor(resolve) { Pe(n - 1); resolve(); });
    else if (armed) stack = new Error('x').stack;
  }
  function Built(n) { this.n = n; if (n > 0) Hop.call(undefined, n - 1); else thrower(); }
  function Hop(n) { return Reflect.construct(Built, [n]); }
  function Mixed(n) {
    this.n = n;
    if (n === 0) thrower();
    else if (n % 2) Reflect.construct(Mixed, [n - 1]);
    else Reflect.apply(Mixed, {}, [n - 1]);
  }
  
  const executors = new Array(DEPTH).fill('executor').join(' < ');
  const built = ['thrower', 'new Built'];
  for (let i = 0; i < DEPTH; i++) built.push('Hop', 'new Built');
  const mixed = ['thrower'];
  for (let n = 0; n <= DEPTH; n++) mixed.push(n === DEPTH || (n + 1) % 2 ? 'new Mixed' : 'Mixed');
  
  for (let round = 0; round < 3; round++) {
    for (let i = 0; i < WARM / 4; i++) { Pe(DEPTH); new Built(DEPTH); new Mixed(DEPTH); }
    Error.stackTraceLimit = 400;
    armed = true;
    let seenBuilt, seenMixed;
    try {
      Pe(DEPTH);
      try { new Built(DEPTH); } catch (error) { seenBuilt = frames(error); }
      try { new Mixed(DEPTH); } catch (error) { seenMixed = frames(error); }
    } finally {
      armed = false;
      Error.stackTraceLimit = limit;
    }
    assert.strictEqual(frames({ stack }), executors, `deep promise executors round ${round}`);
    assert.strictEqual(seenBuilt, built.join(' < '), `deep Reflect.construct through call round ${round}`);
    assert.strictEqual(seenMixed, mixed.join(' < '), `deep alternating construct and apply round ${round}`);
  }
}

{
  let late = false, kept;
  function LateParent(v) {
    this.v = v;
    let s = 0;
    for (let i = 0; i < 3; i++) { s += i; try { s += v.length | 0; } catch (e) { s--; } }
    switch (s & 3) { case 0: this.a = 1; break; case 1: this.b = 2; break; case 2: this.c = 3; break; default: this.d = 4; }
    if (late) kept = new Error('x');
    this.z = [s, s + 1, s + 2].map(function (x) { return x * 2; });
  }
  class LateChild extends LateParent { constructor(v) { super(v); this.w = v + 1; } }
  function LateCaller(v) { const m = new LateChild(v); return m.w; }
  function LateOther(a) { this.a = a; }
  function LateOtherCaller(a) { return new LateOther(a).a; }
  const lateNames = (error) => String(error.stack).split('\n').slice(1, 4).map((line) => /^\s+at ((?:new )?[\w$]+)/.exec(line)?.[1]).join(' < ');

  for (let i = 0; i < 50000; i++) LateCaller(i);
  late = true; LateCaller(2); late = false;
  const early = new Error('x');
  for (let r = 0; r < 4; r++) {
    LateCaller('str');
    for (let i = 0; i < 30000; i++) LateCaller(i % 3 ? 'a' : i % 2 ? 1.5 : i);
    let junk = [];
    for (let i = 0; i < 300000; i++) junk.push({ i });
    junk = null;
  }
  for (let i = 0; i < 100000; i++) LateOtherCaller(i);
  
  assert.strictEqual(lateNames(kept), 'new LateParent < new LateChild < LateCaller', 'stack read after recompilation and collection');
  assert.ok(early.stack.includes('test_stack_new_prefix'), 'stack of an error captured outside compiled code');
}

console.log('ok');
