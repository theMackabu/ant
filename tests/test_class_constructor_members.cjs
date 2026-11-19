const assert = require('node:assert');

const misplaced = [
  'class C { constructor() {} constructor() {} }',
  'class C { "constructor"() {} constructor() {} }',
  'class C { "constructor" = 1; }',
  'class C { constructor = 1; }',
  'class C { get constructor() {} }',
  'class C { async constructor() {} }',
  'class C { *constructor() {} }',
  'class C { static constructor = 1; }',
];
for (const source of misplaced) assert.throws(() => new Function(source), SyntaxError, source);

const allowed = [
  'class C { static constructor() {} constructor() {} }',
  'class C { ["constructor"]() { return 1; } }',
];
for (const source of allowed) assert.doesNotThrow(() => new Function(source), source);

class EvalScopeBase { x() { return 'x'; } static sx() { return 'sx'; } constructor() { this.made = true; } }
class QuotedConstructor extends EvalScopeBase { "constructor"() { super(); this.quoted = true; } }
assert.strictEqual(new QuotedConstructor().quoted, true);
assert.strictEqual(new QuotedConstructor().made, true);

function show(v) {
  if (typeof v === 'function') return 'function ' + v.name;
  if (typeof v === 'symbol' || typeof v === 'bigint') return String(v);
  if (v === undefined) return 'undefined';
  try { return JSON.stringify(v) } catch (e) { return 'unserializable' }
}

function outcome(src) {
  let fn;
  try { fn = new Function(src) } catch (e) { return 'compile ' + e.constructor.name }
  try { return 'value ' + show(fn()) } catch (e) { return 'throw ' + (e && e.constructor ? e.constructor.name : String(e)) }
}

const cases = [
  ["class P { constructor } return Object.getOwnPropertyNames(P).sort().join() + '|' + Object.getOwnPropertyNames(P.prototype).sort().join() + '|' + Object.getOwnPropertyNames(new P()).join()", "compile SyntaxError"],
  ["class P { constructor = 1 } return Object.getOwnPropertyNames(P).sort().join() + '|' + Object.getOwnPropertyNames(P.prototype).sort().join() + '|' + Object.getOwnPropertyNames(new P()).join()", "compile SyntaxError"],
  ["class P { get constructor() { return 1 } } return Object.getOwnPropertyNames(P).sort().join() + '|' + Object.getOwnPropertyNames(P.prototype).sort().join() + '|' + Object.getOwnPropertyNames(new P()).join()", "compile SyntaxError"],
  ["class P { set constructor(v) {} } return Object.getOwnPropertyNames(P).sort().join() + '|' + Object.getOwnPropertyNames(P.prototype).sort().join() + '|' + Object.getOwnPropertyNames(new P()).join()", "compile SyntaxError"],
  ["class P { async constructor() {} } return Object.getOwnPropertyNames(P).sort().join() + '|' + Object.getOwnPropertyNames(P.prototype).sort().join() + '|' + Object.getOwnPropertyNames(new P()).join()", "compile SyntaxError"],
  ["class P { constructor; } return Object.getOwnPropertyNames(P).sort().join() + '|' + Object.getOwnPropertyNames(P.prototype).sort().join() + '|' + Object.getOwnPropertyNames(new P()).join()", "compile SyntaxError"],
  ["class P { static constructor } return Object.getOwnPropertyNames(P).sort().join() + '|' + Object.getOwnPropertyNames(P.prototype).sort().join() + '|' + Object.getOwnPropertyNames(new P()).join()", "compile SyntaxError"],
  ["class P { static constructor = 1 } return Object.getOwnPropertyNames(P).sort().join() + '|' + Object.getOwnPropertyNames(P.prototype).sort().join() + '|' + Object.getOwnPropertyNames(new P()).join()", "compile SyntaxError"],
  ["class P { static constructor; } return Object.getOwnPropertyNames(P).sort().join() + '|' + Object.getOwnPropertyNames(P.prototype).sort().join() + '|' + Object.getOwnPropertyNames(new P()).join()", "compile SyntaxError"],
  ["class P { 'constructor' } return Object.getOwnPropertyNames(P).sort().join() + '|' + Object.getOwnPropertyNames(P.prototype).sort().join() + '|' + Object.getOwnPropertyNames(new P()).join()", "compile SyntaxError"],
  ["class P { 'constructor' = 1 } return Object.getOwnPropertyNames(P).sort().join() + '|' + Object.getOwnPropertyNames(P.prototype).sort().join() + '|' + Object.getOwnPropertyNames(new P()).join()", "compile SyntaxError"],
  ["class P { get 'constructor'() { return 1 } } return Object.getOwnPropertyNames(P).sort().join() + '|' + Object.getOwnPropertyNames(P.prototype).sort().join() + '|' + Object.getOwnPropertyNames(new P()).join()", "compile SyntaxError"],
  ["class P { set 'constructor'(v) {} } return Object.getOwnPropertyNames(P).sort().join() + '|' + Object.getOwnPropertyNames(P.prototype).sort().join() + '|' + Object.getOwnPropertyNames(new P()).join()", "compile SyntaxError"],
  ["class P { *'constructor'() {} } return Object.getOwnPropertyNames(P).sort().join() + '|' + Object.getOwnPropertyNames(P.prototype).sort().join() + '|' + Object.getOwnPropertyNames(new P()).join()", "compile SyntaxError"],
  ["class P { async 'constructor'() {} } return Object.getOwnPropertyNames(P).sort().join() + '|' + Object.getOwnPropertyNames(P.prototype).sort().join() + '|' + Object.getOwnPropertyNames(new P()).join()", "compile SyntaxError"],
  ["class P { async *'constructor'() {} } return Object.getOwnPropertyNames(P).sort().join() + '|' + Object.getOwnPropertyNames(P.prototype).sort().join() + '|' + Object.getOwnPropertyNames(new P()).join()", "compile SyntaxError"],
  ["class P { 'constructor'; } return Object.getOwnPropertyNames(P).sort().join() + '|' + Object.getOwnPropertyNames(P.prototype).sort().join() + '|' + Object.getOwnPropertyNames(new P()).join()", "compile SyntaxError"],
  ["class P { static 'constructor' } return Object.getOwnPropertyNames(P).sort().join() + '|' + Object.getOwnPropertyNames(P.prototype).sort().join() + '|' + Object.getOwnPropertyNames(new P()).join()", "compile SyntaxError"],
  ["class P { static 'constructor' = 1 } return Object.getOwnPropertyNames(P).sort().join() + '|' + Object.getOwnPropertyNames(P.prototype).sort().join() + '|' + Object.getOwnPropertyNames(new P()).join()", "compile SyntaxError"],
  ["class P { static 'constructor'; } return Object.getOwnPropertyNames(P).sort().join() + '|' + Object.getOwnPropertyNames(P.prototype).sort().join() + '|' + Object.getOwnPropertyNames(new P()).join()", "compile SyntaxError"],
  ["class P { construct\\u006Fr } return Object.getOwnPropertyNames(P).sort().join() + '|' + Object.getOwnPropertyNames(P.prototype).sort().join() + '|' + Object.getOwnPropertyNames(new P()).join()", "compile SyntaxError"],
  ["class P { construct\\u006Fr = 1 } return Object.getOwnPropertyNames(P).sort().join() + '|' + Object.getOwnPropertyNames(P.prototype).sort().join() + '|' + Object.getOwnPropertyNames(new P()).join()", "compile SyntaxError"],
  ["class P { get construct\\u006Fr() { return 1 } } return Object.getOwnPropertyNames(P).sort().join() + '|' + Object.getOwnPropertyNames(P.prototype).sort().join() + '|' + Object.getOwnPropertyNames(new P()).join()", "compile SyntaxError"],
  ["class P { set construct\\u006Fr(v) {} } return Object.getOwnPropertyNames(P).sort().join() + '|' + Object.getOwnPropertyNames(P.prototype).sort().join() + '|' + Object.getOwnPropertyNames(new P()).join()", "compile SyntaxError"],
  ["class P { async construct\\u006Fr() {} } return Object.getOwnPropertyNames(P).sort().join() + '|' + Object.getOwnPropertyNames(P.prototype).sort().join() + '|' + Object.getOwnPropertyNames(new P()).join()", "compile SyntaxError"],
  ["class P { construct\\u006Fr; } return Object.getOwnPropertyNames(P).sort().join() + '|' + Object.getOwnPropertyNames(P.prototype).sort().join() + '|' + Object.getOwnPropertyNames(new P()).join()", "compile SyntaxError"],
  ["class P { static construct\\u006Fr } return Object.getOwnPropertyNames(P).sort().join() + '|' + Object.getOwnPropertyNames(P.prototype).sort().join() + '|' + Object.getOwnPropertyNames(new P()).join()", "compile SyntaxError"],
  ["class P { static construct\\u006Fr = 1 } return Object.getOwnPropertyNames(P).sort().join() + '|' + Object.getOwnPropertyNames(P.prototype).sort().join() + '|' + Object.getOwnPropertyNames(new P()).join()", "compile SyntaxError"],
  ["class P { static construct\\u006Fr; } return Object.getOwnPropertyNames(P).sort().join() + '|' + Object.getOwnPropertyNames(P.prototype).sort().join() + '|' + Object.getOwnPropertyNames(new P()).join()", "compile SyntaxError"],
  ["class P { constructor() {} constructor() {} }", "compile SyntaxError"],
  ["class P { constructor() {} 'constructor'() {} }", "compile SyntaxError"],
  ["class P { 'constructor'(a) { this.a = a } } return new P(3).a", "value 3"],
  ["class P { get constructor() {} }", "compile SyntaxError"],
];

const failed = [];
for (const [src, want] of cases) {
  const got = outcome(src);
  if (got !== want) failed.push(`${src} => ${got}, want ${want}`);
}
assert.deepStrictEqual(failed, []);

console.log('ok');
