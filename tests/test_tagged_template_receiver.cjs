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

console.log('ok');
