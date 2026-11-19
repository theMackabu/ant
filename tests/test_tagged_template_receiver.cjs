const assert = require('node:assert');

const object = {
  tag(strings, ...values) {
    return [this === object, strings.raw.join('|'), values];
  }
};

assert.deepStrictEqual(object.tag`a${1}b`, [true, 'a|b', [1]]);
assert.deepStrictEqual(object['tag']`x`, [true, 'x', []]);

class Store {
  #seen = [];

  #record(strings) {
    this.#seen.push(strings.join('?'));
    return this.#seen.length;
  }

  get(strings, ...values) {
    return [this.#record(strings), values];
  }
}

const store = new Store();
assert.deepStrictEqual(store.get`select ${1}`, [1, [1]]);
assert.deepStrictEqual(store.get`select ${2}, ${3}`, [2, [2, 3]]);

class Base {
  tag(strings) {
    return [this instanceof Derived, strings[0]];
  }
}

class Derived extends Base {
  viaSuper() {
    return super.tag`from super`;
  }
}

assert.deepStrictEqual(new Derived().viaSuper(), [true, 'from super']);

function plain(strings) {
  'use strict';
  return [this, strings[0]];
}

assert.deepStrictEqual(plain`free`, [undefined, 'free']);

const nested = { inner: { tag(strings) { return this === nested.inner && strings.length === 1; } } };
assert.strictEqual(nested.inner.tag`deep`, true);

const sites = [];
for (let i = 0; i < 2; i++) sites.push(object.tag`same`[1], ((s) => s)`site`);
assert.strictEqual(sites[1], sites[3]);

const tagged = { v: 7, tag(strings, ...values) { return [this && this.v, strings.raw.join('|'), values.join()].join(':'); } };
class TagBase { tag(strings) { return [this.v, strings.raw.join('|')].join(':'); } }
class TagDerived extends TagBase {
  constructor() { super(); this.v = 9; }
  viaSuper() { return super.tag`a${1}b`; }
  viaComputedSuper() { return super['tag']`x`; }
}
assert.strictEqual(tagged.tag`a${1}b`, '7:a|b:1');
assert.strictEqual(tagged['tag']`q`, '7:q:');
assert.strictEqual((0, tagged.tag)`z`, ':z:');
assert.strictEqual(new TagDerived().viaSuper(), '9:a|b');
assert.strictEqual(new TagDerived().viaComputedSuper(), '9:x');

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
  ["var o = { F(s) { return this === o } }; return (o?.F)`x`", "value true"],
  ["var o = { F(s) { return this === o } }; return (o.F)`x`", "value true"],
  ["var o = { p: { F(s) { return this === o.p } } }; return (o?.p.F)`x`", "value true"],
];

const failed = [];
for (const [src, want] of cases) {
  const got = outcome(src);
  if (got !== want) failed.push(`${src} => ${got}, want ${want}`);
}
assert.deepStrictEqual(failed, []);

console.log('ok');
