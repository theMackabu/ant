const assert = require('node:assert');

class Base {
  constructor(...args) {
    this.args = args;
    this.count = arguments.length;
    this.target = new.target.name;
  }
}

class Fields extends Base {
  tag = 'f';
}

class Deeper extends Fields {
  extra = this.count;
}

for (let i = 0; i < 5000; i++) {
  const none = new Fields();
  assert.deepStrictEqual(none.args, []);
  assert.strictEqual(none.count, 0);

  const many = new Fields(i, 'b', undefined, null, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20);
  assert.strictEqual(many.count, 20);
  assert.strictEqual(many.args[0], i);
  assert.strictEqual(many.args[2], undefined);
  assert.strictEqual(many.args[19], 20);
  assert.strictEqual(many.tag, 'f');
  assert.strictEqual(many.target, 'Fields');

  const deep = new Deeper(1, 2, 3);
  assert.deepStrictEqual(deep.args, [1, 2, 3]);
  assert.strictEqual(deep.extra, 3);
  assert.strictEqual(deep.target, 'Deeper');
}

function Legacy(a, b) {
  this.sum = a + b;
}
class FromLegacy extends Legacy {
  ready = this.sum * 2;
}
for (let i = 0; i < 3000; i++) assert.strictEqual(new FromLegacy(i, 1).ready, (i + 1) * 2);

const replaced = { replaced: true };
class Replacer {
  constructor() { return replaced; }
}
class FromReplacer extends Replacer {
  added = 1;
}
for (let i = 0; i < 3000; i++) {
  const r = new FromReplacer(1, 2);
  assert.strictEqual(r, replaced);
  assert.strictEqual(r.added, 1);
}

class Thrower {
  constructor(x) { if (x) throw new RangeError('bad ' + x); }
}
class FromThrower extends Thrower {
  field = 1;
}
for (let i = 0; i < 3000; i++) {
  assert.strictEqual(new FromThrower(0).field, 1);
  assert.throws(() => new FromThrower(i + 1), { name: 'RangeError', message: 'bad ' + (i + 1) });
}

class Items extends Array {
  label = 'items';
}
for (let i = 0; i < 3000; i++) {
  const items = new Items(1, 2, i);
  assert.strictEqual(items.length, 3);
  assert.strictEqual(items[2], i);
  assert.strictEqual(items.label, 'items');
  assert.ok(items instanceof Items);
}

const iterator = Array.prototype[Symbol.iterator];
Array.prototype[Symbol.iterator] = function* () { yield 'patched'; };
try {
  const patched = new Fields(1, 2);
  assert.deepStrictEqual(patched.args, [1, 2]);
} finally {
  Array.prototype[Symbol.iterator] = iterator;
}

console.log('PASS super forward');
