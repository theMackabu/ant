const assert = require('node:assert');

const run = (source) => (0, eval)(source);

assert.strictEqual(run('({v\\u0061r: 1}).var'), 1);
assert.strictEqual(run('({\\u0069f: 2}).if'), 2);
assert.strictEqual(run('({cl\\u0061ss() { return 3; }}).class()'), 3);
assert.strictEqual(run('({get v\\u0061r() { return 4; }}).var'), 4);
assert.strictEqual(run('var o = {set v\\u0061r(x) { this.q = x; }}; o.var = 5; o.q'), 5);
assert.strictEqual(run('({async f\\u006fr() {}}).for.name'), 'for');
assert.strictEqual(run('({*f\\u006fr() {}}).for.name'), 'for');
assert.strictEqual(run('var o = {var: 6}; var {v\\u0061r: q} = o; q'), 6);
assert.strictEqual(run('var o = {var: 7}; o.v\\u0061r'), 7);
assert.strictEqual(run('var o = {var: 8}; o?.v\\u0061r'), 8);
assert.strictEqual(run('var o = {}; o.n\\u0065w = 9; o.new'), 9);
assert.strictEqual(run('class EscA { static f\\u006fr() { return 10; } } EscA.for()'), 10);
assert.strictEqual(run('class EscB { v\\u0061r = 11; } new EscB().var'), 11);
assert.strictEqual(run('class EscC { get \\u0069f() { return 12; } } new EscC().if'), 12);
assert.strictEqual(run('({tru\\u0065: 13}).true'), 13);
assert.strictEqual(run('var a = {b: {t\\u0068is: 14}}; a.b.this'), 14);
assert.strictEqual(run('class EscD { #v\\u0061r = 16; g() { return this.#var; } } new EscD().g()'), 16);

for (const source of [
  'var v\\u0061r = 1', 'v\\u0061r x = 1', 'l\\u0065t escLet = 15', '({v\\u0061r})', 'var {v\\u0061r} = {}', 'function f(v\\u0061r) {}', 'v\\u0061r',
  'x = cl\\u0061ss {}', 'tru\\u0065', 'nul\\u006c', 'thi\\u0073', 'f\\u006fr (;;) break',
]) assert.throws(() => run(source), SyntaxError, source);

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
  ["class A { #\\u0061b = 1; g() { return this.#ab } } return new A().g()", "value 1"],
  ["class A { #a\\u{62} = 2; g() { return this.#ab } } return new A().g()", "value 2"],
  ["class A { #ab = 3; g() { return this.#\\u0061b } } return new A().g()", "value 3"],
];

const failed = [];
for (const [src, want] of cases) {
  const got = outcome(src);
  if (got !== want) failed.push(`${src} => ${got}, want ${want}`);
}
assert.deepStrictEqual(failed, []);

console.log('ok');
