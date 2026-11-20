const assert = require('node:assert');

class Pair {
  #x = 1;
  #y = 2;
  #rest = null;
  #log = [];
  static #count = 0;
  static #total = 0;

  get pair() { return [this.#x, this.#y]; }
  get rest() { return this.#rest; }
  get log() { return this.#log; }

  set #traced(v) { this.#log.push(v); }
  get #readonly() { return 1; }
  #method() {}

  swap() {
    [this.#x, this.#y] = [this.#y, this.#x];
    return this;
  }
  static swap(a) {
    [a.#x, a.#y] = [a.#y, a.#x];
    return a;
  }
  static swapValue(a) {
    return [a.#x, a.#y] = [a.#y, a.#x];
  }
  static fromObject(a, src) {
    ({ x: a.#x, y: a.#y } = src);
    return a;
  }
  static fromNested(a, src) {
    [[a.#x], { deep: { y: a.#y } }] = src;
    ({ list: [a.#rest, ...a.#log] } = src[1]);
    return a;
  }
  static withDefaults(a, arr, obj) {
    [a.#x = 10, a.#y = 20] = arr;
    ({ r: a.#rest = 'none' } = obj);
    return a;
  }
  static arrayRest(a, src) {
    [a.#x, ...a.#rest] = src;
    return a;
  }
  static objectRest(a, src) {
    ({ x: a.#x, ...a.#rest } = src);
    return a;
  }
  static computedKey(a, key, src) {
    ({ [key]: a.#y } = src);
    return a;
  }
  static accessor(a, src) {
    [a.#traced, { v: a.#traced }, ...a.#traced] = src;
    return a;
  }
  static statics(src) {
    [Pair.#count, { total: Pair.#total = 7 }] = src;
    return [Pair.#count, Pair.#total];
  }
  static toMethod(a) {
    [a.#method] = [1];
  }
  static toReadonly(a) {
    ({ v: a.#readonly } = { v: 1 });
  }
  static toForeign(o) {
    [o.#x] = [1];
  }
  static forOf(a, src) {
    let sum = 0;
    for (a.#x of src) sum += a.#x;
    return sum;
  }
  static forIn(a, src) {
    const keys = [];
    for (a.#y in src) keys.push(a.#y);
    return keys;
  }
  static forOfPattern(a, src) {
    let sum = 0;
    for ([a.#x, { y: a.#y = 5 }] of src) sum += a.#x * a.#y;
    return sum;
  }
  static forInPattern(a, src) {
    let out = '';
    for ([a.#x, ...a.#rest] in src) out += a.#x + a.#rest.join('');
    return out;
  }
  static async forAwait(a, src) {
    let sum = 0;
    for await (a.#x of src) sum += a.#x;
    return sum;
  }
  static *generator(a, src) {
    for ([a.#x, a.#y] of src) yield a.#x + a.#y;
  }
  static sequence(a) {
    let n = 0;
    [a.#x, a.#y] = [n++, n++];
    return (([a.#x] = [a.#y + n]), a.#x) + n;
  }
  static closure(a) {
    return () => {
      [a.#x, a.#y] = [a.#y, a.#x];
      return a;
    };
  }
}

assert.deepStrictEqual(new Pair().swap().pair, [2, 1]);
assert.deepStrictEqual(Pair.swap(new Pair()).pair, [2, 1]);
assert.deepStrictEqual(Pair.swap(Pair.swap(new Pair())).pair, [1, 2]);
assert.deepStrictEqual(Pair.swapValue(new Pair()), [2, 1]);
assert.deepStrictEqual(Pair.fromObject(new Pair(), { x: 'a', y: 'b' }).pair, ['a', 'b']);

{
  const p = Pair.fromNested(new Pair(), [[3], { deep: { y: 4 }, list: [5, 6, 7] }]);
  assert.deepStrictEqual(p.pair, [3, 4]);
  assert.strictEqual(p.rest, 5);
  assert.deepStrictEqual(p.log, [6, 7]);
}

{
  const p = Pair.withDefaults(new Pair(), [undefined, 0], {});
  assert.deepStrictEqual(p.pair, [10, 0]);
  assert.strictEqual(p.rest, 'none');
  assert.deepStrictEqual(Pair.withDefaults(new Pair(), [], { r: null }).pair, [10, 20]);
  assert.strictEqual(Pair.withDefaults(new Pair(), [], { r: null }).rest, null);
}

{
  const p = Pair.arrayRest(new Pair(), 'abc');
  assert.deepStrictEqual(p.pair, ['a', 2]);
  assert.deepStrictEqual(p.rest, ['b', 'c']);
  const q = Pair.objectRest(new Pair(), { x: 1, y: 2, z: 3 });
  assert.deepStrictEqual(q.pair, [1, 2]);
  assert.deepStrictEqual(q.rest, { y: 2, z: 3 });
  assert.deepStrictEqual(Pair.computedKey(new Pair(), 'k', { k: 9 }).pair, [1, 9]);
}

assert.deepStrictEqual(Pair.accessor(new Pair(), [1, { v: 2 }, 3, 4]).log, [1, 2, [3, 4]]);
assert.deepStrictEqual(Pair.statics([3, {}]), [3, 7]);
assert.deepStrictEqual(Pair.statics([4, { total: 8 }]), [4, 8]);

assert.throws(() => Pair.toMethod(new Pair()), TypeError);
assert.throws(() => Pair.toReadonly(new Pair()), TypeError);
assert.throws(() => Pair.toForeign({}), TypeError);
assert.throws(() => Pair.toForeign(null), TypeError);
assert.throws(() => Pair.swap({}), TypeError);

{
  const p = new Pair();
  assert.strictEqual(Pair.forOf(p, [1, 2, 3]), 6);
  assert.deepStrictEqual(p.pair, [3, 2]);
  assert.deepStrictEqual(Pair.forIn(p, { a: 1, b: 2 }), ['a', 'b']);
  assert.deepStrictEqual(p.pair, [3, 'b']);
  assert.strictEqual(Pair.forOfPattern(p, [[1, {}], [2, { y: 3 }]]), 11);
  assert.deepStrictEqual(p.pair, [2, 3]);
  assert.strictEqual(Pair.forInPattern(p, { abc: 1, de: 2 }), 'abcde');
  assert.deepStrictEqual([...Pair.generator(p, [[1, 2], [3, 4]])], [3, 7]);
  assert.strictEqual(Pair.sequence(p), 5);
  assert.deepStrictEqual(p.pair, [3, 1]);
  assert.deepStrictEqual(Pair.closure(p)().pair, [1, 3]);
}

{
  const p = new Pair();
  const big = [];
  for (let i = 0; i < 20000; i++) big.push(i);
  assert.strictEqual(Pair.forOf(p, big), 199990000);
  for (let i = 0; i < 20001; i++) Pair.swap(p);
  assert.deepStrictEqual(p.pair, [2, 19999]);
  let sum = 0;
  for (let i = 0; i < 20000; i++) sum += Pair.swapValue(p)[0];
  assert.strictEqual(sum, 200010000);
}

Pair.forAwait(new Pair(), [1, Promise.resolve(2), 3]).then((sum) => {
  assert.strictEqual(sum, 6);
  console.log('ok');
});
