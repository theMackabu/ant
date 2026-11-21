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
class SpreadChild extends Parent { constructor(a) { super(...one, a); this.c = 1; } }
class SpreadPlainChild extends PlainParent { constructor(a) { super(...one, a); this.c = 1; } }
const BoundPlain = Plain.bind(null);
const one = [1];

const cases = {
  'new with a spread before the last argument': [(a) => new Plain(...one, a), 'new Plain'],
  'call with a spread before the last argument': [(a) => Plain.call({}, ...one, a), 'Plain'],
  'function call with a spread before the last argument': [(a) => helper(...one, a), 'thrower < helper'],
  'super with a spread before the last argument': [(a) => new SpreadChild(a), 'new Parent < new SpreadChild'],
  'super into function with a spread before the last argument': [(a) => new SpreadPlainChild(a), 'new PlainParent < new SpreadPlainChild'],
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

console.log('ok');
