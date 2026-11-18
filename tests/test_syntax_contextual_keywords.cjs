const assert = require('node:assert');
const vm = require('node:vm');

function parses(src) {
  try { new vm.Script(src); return true } catch (e) {
    if (e instanceof SyntaxError) return false;
    throw e;
  }
}

const valid = [
  "(yield);",
  "({ [yield]: 1 });",
  "({ yield: yield });",
  "async\nyield;",
  "async function f() { (function await() {}) }",
  "async function w() { (function await() {});\n}",
  "async function w() { (function* await() {});\n}",
  "async function w() { (yield);\n}",
  "async function w() { ({ [yield]: 1 });\n}",
  "async function w() { ({ yield: yield });\n}",
  "async function w() { delete yield.x;\n}",
  "async function w() { f(yield);\n}",
  "async function w() { new yield;\n}",
  "async function w() { return yield;\n}",
  "async function w() { throw yield;\n}",
  "async function w() { try { yield\nx; } catch (e) {}\n}",
  "async function w() { typeof yield;\n}",
  "async function w() { yield\nof;\n}",
  "async function w() { yield\nx;\n}",
  "async function w() { yield\nyield;\n}",
  "async function w() { yield[0];\n}",
  "async function w() { { yield\nx; }\n}",
  "async function* w() { (function await() {});\n}",
  "async function* w() { (function* await() {});\n}",
  "await\nyield;",
  "delete yield.x;",
  "f(yield);",
  "function w() { return yield;\n}",
  "function w() { yield\nawait;\n}",
  "new yield;",
  "throw yield;",
  "try { yield\nx; } catch (e) {}",
  "typeof yield;",
  "var aw\\u0061it",
  "yield",
  "yield\n(1);",
  "yield\n.x;",
  "yield\n/x/g;",
  "yield\n`x`;",
  "yield\nawait;",
  "yield\nin {};",
  "yield\nof;",
  "yield\nx;",
  "yield\nyield;",
  "yield (1);",
  "yield * 2;",
  "yield /1/ 1;",
  "yield `x`;",
  "yield in {};",
  "yield instanceof Object;",
  "yield instanceof yield;",
  "yield || 1;",
  "yield();",
  "yield, x;",
  "yield,1;",
  "yield.x;",
  "yield;",
  "yield;\nyield;",
  "yield;;",
  "yield?.x;",
  "yield[0];",
  "yield`a`;",
  "{ yield\nx; }",
];

const invalid = [
  "\"use strict\"; (async function await() {});",
  "(async function await() {})",
  "(async function await() {});",
  "(function eval() { \"use strict\" })",
  "(function* yield() {})",
  "(function* yield() {});",
  "async function* w() { function yield() {}\n}",
  "async function* w() { function* yield() {}\n}",
  "async function* w() { try {} catch (yield) {}\n}",
  "async function* w() { var yield;\n}",
  "async function* w() { var z, yield;\n}",
  "delete super.x;",
  "function eval() { \"use strict\" }",
  "function* f(yield) {}",
  "function* g() { (function* yield() {}) }",
  "function* g() { function yield() {} }",
  "function* g() { try {} catch (yield) {} }",
  "function* g() { var yield }",
  "function* g() { void yield }",
  "function* g() { yield\n* 2 }",
  "function* w() { (function* yield() {});\n}",
  "function* w() { function yield() {}\n}",
  "function* w() { function* f(yield) {}\n}",
  "function* w() { function* yield() {}\n}",
  "function* w() { try {} catch (yield) {}\n}",
  "function* w() { var yield;\n}",
  "function* w() { var z, yield;\n}",
];

for (const src of valid) assert.strictEqual(parses(src), true, 'should parse: ' + JSON.stringify(src));
for (const src of invalid) assert.strictEqual(parses(src), false, 'should be a SyntaxError: ' + JSON.stringify(src));

const run = src => new Function(src)();
assert.strictEqual(run('var l\\u0065t = 2, yi\\u0065ld = 3, aw\\u0061it = 4; return l\\u0065t + yield + await'), 9);

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
  ["'use strict'; yield: 1", "compile SyntaxError"],
  ["async function f() { await: 1 }", "compile SyntaxError"],
  ["function* g() { yield: 1 }", "compile SyntaxError"],
];

const failed = [];
for (const [src, want] of cases) {
  const got = outcome(src);
  if (got !== want) failed.push(`${src} => ${got}, want ${want}`);
}
assert.deepStrictEqual(failed, []);

console.log('ok');
