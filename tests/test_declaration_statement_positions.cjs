const assert = require('node:assert');

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
  ["foo: class C {}", "compile SyntaxError"],
  ["foo: let x", "compile SyntaxError"],
  ["foo: const x = 1", "compile SyntaxError"],
  ["'use strict'; foo: function f() {}", "compile SyntaxError"],
  ["foo: function f() {} return typeof f", "value \"function\""],
  ["if (1) foo: function f() {}", "compile SyntaxError"],
  ["foo: function* g() {}", "compile SyntaxError"],
  ["foo: async function f() {}", "compile SyntaxError"],
  ["foo: bar: function f() {} return 1", "value 1"],
  ["if (1) function f() {} return typeof f", "value \"function\""],
  ["'use strict'; if (1) function f() {}", "compile SyntaxError"],
  ["if (1) class C {}", "compile SyntaxError"],
  ["if (1) function* g() {}", "compile SyntaxError"],
  ["if (1) async function f() {}", "compile SyntaxError"],
  ["if (0) ; else function f() {} return typeof f", "value \"function\""],
  ["while (0) function f() {}", "compile SyntaxError"],
  ["for (;;) function f() {}", "compile SyntaxError"],
  ["foo: { function f() {} } return typeof f", "value \"function\""],
];

const failed = [];
for (const [src, want] of cases) {
  const got = outcome(src);
  if (got !== want) failed.push(`${src} => ${got}, want ${want}`);
}
assert.deepStrictEqual(failed, []);

console.log('ok');
