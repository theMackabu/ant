const assert = require('node:assert');

const cases = [
  "let a, b; ({ __proto__: a, __proto__: b } = { __proto__: { t: 1 } }); a.t + b.t",
  "let a, b; ({ '__proto__': a, __proto__: b } = { __proto__: { t: 2 } }); a.t + b.t",
  "let a; ({ a = 5 } = {}); a",
  "let a; [{ a = 6 }] = [{}]; a",
  "let r; for ({ a: r = 7 } of [{}]) ; r",
  "let q; for ({ q = 8 } of [{}]) ; q",
  "(({ a = 9 }) => a)({})",
  "(({ __proto__: x, __proto__: y }) => x === y)({})",
  "const { a = 10 } = {}; a",
  "let z; ({ x: { z = 11 } } = { x: {} }); z",
  "({ a = 1 })",
  "({ __proto__: 1, __proto__: 2 })",
  "({ '__proto__': {}, \"__proto__\": {} })",
  "[{ a = 1 }]",
  "f({ a = 1 }); function f() {}",
  "({ a: b = 1 }).a",
  "var b; ({ a: b = 1 }); b",
  "({ ['__proto__']: 1, __proto__: null, ['__proto__']: 2 }).__proto__",
  "Object.getPrototypeOf({ '\\x5f_proto__': Array.prototype }) === Array.prototype",
  "Object.getPrototypeOf({ __proto__: 'str' }) === Object.prototype",
];

const expected = [
  "2",
  "4",
  "5",
  "6",
  "7",
  "8",
  "9",
  "true",
  "10",
  "11",
  "SyntaxError: Invalid shorthand property initializer",
  "SyntaxError: Duplicate __proto__ fields are not allowed in object literals",
  "SyntaxError: Duplicate __proto__ fields are not allowed in object literals",
  "SyntaxError: Invalid shorthand property initializer",
  "SyntaxError: Invalid shorthand property initializer",
  "1",
  "1",
  "2",
  "true",
  "true",
];

cases.forEach((src, i) => {
  let out;
  try { out = JSON.stringify((0, eval)(src)); } catch (e) { out = e.constructor.name + ': ' + e.message; }
  assert.strictEqual(out, expected[i], src);
});

const base = { tag: 'base' };
function quoted(v) { return { '__proto__': v, k: 1 }; }
function doubleQuoted(v) { return { "__proto__": v }; }
function ident(v) { return { __proto__: v }; }
let hits = 0;
for (let i = 0; i < 30000; i++) {
  const v = i % 3 === 0 ? base : i % 3 === 1 ? null : 5;
  for (const make of [quoted, doubleQuoted, ident]) {
    const o = make(v);
    assert.strictEqual(Object.prototype.hasOwnProperty.call(o, '__proto__'), false);
    assert.strictEqual(Object.getPrototypeOf(o), v === 5 ? Object.prototype : v);
    hits++;
  }
}
assert.strictEqual(hits, 90000);
console.log('PASS object literal __proto__ keys');
