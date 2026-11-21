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
  ["return Object.keys({ 08() {} })", "value [\"8\"]"],
  ["return Object.keys({ 08: 1 })", "value [\"8\"]"],
  ["return Object.keys({ 010: 1 })", "value [\"8\"]"],
  ["return Object.keys({ 010() {} })", "value [\"8\"]"],
  ["return Object.keys({ 09.5: 1 })", "value [\"9.5\"]"],
  ["return Object.keys({ get 08() { return 1 } })", "value [\"08\"]"],
  ["return Object.keys({ 0x10: 1, 1e3: 2, .5: 3, 1.0: 4, 0b11: 5, 0o17: 6, 1_0: 7 })", "value [\"1\",\"3\",\"10\",\"15\",\"16\",\"1000\",\"0.5\"]"],
  ["return ({ 08: 'a' })[8]", "value \"a\""],
  ["return ({ 010: 'a' })[8]", "value \"a\""],
  ["var o = { 08() { return 1 } }; return [o[8] && o[8].name, o['08'] && o['08'].name]", "value [\"08\",null]"],
  ["var o = { get 010() { return 1 } }; return Object.getOwnPropertyNames(o)", "value [\"8\"]"],
  ["return Object.keys({ 1n: 1, 0x1Fn: 2 })", "value [\"1\",\"31\"]"],
  ["return Object.keys({ 1e21: 1, 1e-7: 2, 123456789012345680000: 3, 0.1: 4 })", "value [\"1e+21\",\"1e-7\",\"123456789012345680000\",\"0.1\"]"],
  ["return Object.keys({ 00: 1 })", "value [\"0\"]"],
  ["return Object.keys({ 07: 1, 08: 2, 09: 3 })", "value [\"7\",\"8\",\"9\"]"],
  ["return Object.keys({ 0008: 1, 0010: 2 })", "value [\"8\"]"],
  ["var o = { 08() { return 1 } }; return [Object.keys(o), o[8].name]", "value [[\"8\"],\"08\"]"],
  ["var o = { get 08() { return 1 }, set 09(v) {} }; return [Object.getOwnPropertyNames(o), Object.getOwnPropertyDescriptor(o, '08').get.name]", "value [[\"08\",\"09\"],\"get 08\"]"],
  ["var o = { 0899999999: function () {}, 08999999999: function () {}, 0008: () => {}, 018: function () {} }; return Object.keys(o).map(k => k + ':' + o[k].name)", "value [\"8:8\",\"18:18\",\"899999999:0899999999\",\"8999999999:8999999999\"]"],
  ["return [0008, 0010, 0018, 08.5, 0089, 010]", "value [8,8,18,8.5,89,8]"],
];

const failed = [];
for (const [src, want] of cases) {
  const got = outcome(src);
  if (got !== want) failed.push(`${src} => ${got}, want ${want}`);
}
assert.deepStrictEqual(failed, []);

console.log('ok');
