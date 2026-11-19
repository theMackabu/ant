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
  ["var n = 0; var k = { toString() { n++; return 'ts' + n } }; class C { [k] = () => {} } return [Object.keys(new C()), Object.keys(new C()), n, Object.values(new C())[0].name]", "value [[\"ts1\"],[\"ts1\"],1,\"ts1\"]"],
  ["var n = 0; var k = { toString() { n++; return 'ts' + n } }; class C { static [k] = () => {} } return [Object.keys(C), n, Object.values(C)[0].name]", "value [[\"ts1\"],1,\"ts1\"]"],
  ["var n = 0; var k = { toString() { n++; return 'ts' + n } }; class C { [k]() {} } return [Object.getOwnPropertyNames(C.prototype), n, C.prototype.ts1.name]", "value [[\"constructor\",\"ts1\"],1,\"ts1\"]"],
  ["var n = 0; var k = { toString() { n++; return 'ts' + n } }; class C { get [k]() { return 1 } static set [k](v) {} } return [Object.getOwnPropertyNames(C.prototype), Object.getOwnPropertyNames(C).sort(), n]", "value [[\"constructor\",\"ts1\"],[\"length\",\"name\",\"prototype\",\"ts2\"],2]"],
  ["var s = Symbol('q'); class C { [s] = () => {}; static [s] = () => {} } return [new C()[s].name, C[s].name]", "value [\"[q]\",\"[q]\"]"],
  ["class C { [1 + 1] = () => {}; [null] = function () {}; [undefined] = class {} } var c = new C(); return [c[2].name, c.null.name, c.undefined.name]", "value [\"2\",\"null\",\"undefined\"]"],
  ["var n = 0; var k = { [Symbol.toPrimitive]() { return 'p' + (++n) } }; var o = { [k]: 1, [k]() {} }; return [Object.keys(o), n]", "value [[\"p1\",\"p2\"],2]"],
  ["var log = []; var K = class { static [(log.push('k1'), 'a')] = log.push('i1'); [(log.push('k2'), 'b')]() {} }; return log", "value [\"k1\",\"k2\",\"i1\"]"],
  ["var log = []; class K { static [(log.push('a'), 'a')] = log.push('ia'); [(log.push('b'), 'b')]() {} ; [(log.push('c'), 'c')] = 1; static [(log.push('d'), 'd')]() {} ; get [(log.push('e'), 'e')]() { return 1 } static set [(log.push('f'), 'f')](v) {} static { log.push('sb') } static [(log.push('g'), 'g')] = log.push('ig') } new K(); return log", "value [\"a\",\"b\",\"c\",\"d\",\"e\",\"f\",\"g\",\"ia\",\"sb\",\"ig\"]"],
  ["var log = []; var K = class { static [(log.push('a'), 'a')] = log.push('ia'); [(log.push('b'), 'b')]() {} ; [(log.push('c'), 'c')] = 1; static [(log.push('d'), 'd')]() {} ; get [(log.push('e'), 'e')]() { return 1 } static set [(log.push('f'), 'f')](v) {} static { log.push('sb') } static [(log.push('g'), 'g')] = log.push('ig') }; new K(); return log", "value [\"a\",\"b\",\"c\",\"d\",\"e\",\"f\",\"g\",\"ia\",\"sb\",\"ig\"]"],
  ["var log = []; var K = class extends (log.push('h'), Object) { [(log.push('a'), 'a')]() {} static async *[(log.push('b'), 'b')]() {} async [(log.push('c'), 'c')]() {} *[(log.push('d'), 'd')]() {} }; return log", "value [\"h\",\"a\",\"b\",\"c\",\"d\"]"],
  ["var log = []; (class { [(log.push('a'), 'a')]() {} static [(log.push('b'), 'b')]() {} }); return log", "value [\"a\",\"b\"]"],
  ["var log = []; var f = () => class { [(log.push('a'), 'a')]() {} static [(log.push('b'), 'b')] = 1 }; f(); f(); return log", "value [\"a\",\"b\",\"a\",\"b\"]"],
  ["var K = class { static ['a'] = 1; ['b']() { return 2 } static ['c']() { return 3 } }; return [K.a, new K().b(), K.c()]", "value [1,2,3]"],
  ["function f() { return class { ['x' + arguments.length]() { return 1 } } } return Object.getOwnPropertyNames(f(1, 2).prototype)", "value [\"constructor\",\"x2\"]"],
];

const failed = [];
for (const [src, want] of cases) {
  const got = outcome(src);
  if (got !== want) failed.push(`${src} => ${got}, want ${want}`);
}
assert.deepStrictEqual(failed, []);

console.log('ok');
