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
  ["class C { static ['k'] = class { static n = this.name } } return C.k.n", "value \"k\""],
  ["class C { static 1 = class { static n = this.name } } return C[1].n", "value \"1\""],
  ["class C { ['k'] = class { static n = this.name } } return new C().k.n", "value \"k\""],
  ["class C { static k = class { static n = this.name } } return C.k.n", "value \"k\""],
  ["class C { static ['k'] = class { static name = 'own' } } return C.k.name", "value \"own\""],
  ["var s = Symbol('d'); class C { static [s] = class { static n = this.name } } return C[s].n", "value \"[d]\""],
  ["class C { static ['k'] = class Named { static n = this.name } } return C.k.n", "value \"Named\""],
  ["class C { 0x10 = class { static n = this.name } } return new C()[16].n", "value \"16\""],
  ["class C { static ['k'] = (class { static n = this.name }) } return C.k.n", "value \"k\""],
  ["class C { static ['k'] = [class { static n = this.name }][0] } return C.k.n", "value \"\""],
  ["class C { static [1 + 1] = class { static { this.n = this.name } } } return C[2].n", "value \"2\""],
];

const failed = [];
for (const [src, want] of cases) {
  const got = outcome(src);
  if (got !== want) failed.push(`${src} => ${got}, want ${want}`);
}
assert.deepStrictEqual(failed, []);

console.log('ok');
