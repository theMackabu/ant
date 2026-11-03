const assert = require('node:assert');

const base = { toString() { return 'base'; }, tag: 'B' };
function withProto(v) { return { __proto__: base, own: v }; }
function plain(v) { return { own: v }; }
function str(o) { return o.toString(); }
function tag(o) { return o.tag; }
function nullProto(v) { return { __proto__: null, own: v }; }
function hasToString(o) { return 'toString' in o; }

for (let i = 0; i < 100000; i++) {
  str(plain(i));
  tag(plain(i));
  hasToString(plain(i));
  withProto(i);
  nullProto(i);
}

const o = withProto(1);
assert.strictEqual(Object.getPrototypeOf(o), base);
assert.strictEqual(str(o), 'base');
assert.strictEqual(tag(o), 'B');
assert.strictEqual(String(o), 'base');
assert.strictEqual(hasToString(nullProto(2)), false);
assert.strictEqual(Object.getPrototypeOf(nullProto(3)), null);
assert.strictEqual(str(plain(4)), '[object Object]');
console.log('ok');
